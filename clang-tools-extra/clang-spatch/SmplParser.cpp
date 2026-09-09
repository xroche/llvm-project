//===--- SmplParser.cpp - Parser for a subset of SmPL ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SmPL is read line by line rather than through a token stream, because two of
// its rules are stated in terms of physical lines and cannot be expressed in a
// context-free grammar:
//
//  * A '-', '+' or '*' marker, and the '(', '|', ')' of a disjunction, and the
//    '@' that opens or closes a rule header, count only at physical column 0.
//  * A 'when' clause ends at the end of its physical line. Coccinelle inserts
//    a synthetic end-of-line token after the last token that shares the line
//    of the 'when' that opened the clause, so two 'when' clauses on one line
//    are a parse error while the same two on separate lines are not.
//
//===----------------------------------------------------------------------===//

#include "SmplParser.h"
#include "PatternParser.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

using namespace llvm;

namespace clang::spatch {
namespace {

/// SemanticPatch names three members after their own enum type, which hides
/// the type name at class scope, so the types are recovered here once.
using MetaKind = decltype(MetaVar::Kind);
using ItemKind = decltype(PatternItem::Kind);
using ItemMarker = decltype(PatternItem::Marker);

bool isIdentStart(char C) { return isAlpha(C) || C == '_'; }
bool isIdentCont(char C) { return isAlnum(C) || C == '_'; }

/// The leading identifier of \p S, empty if it does not start with one.
StringRef takeIdent(StringRef S) {
  if (S.empty() || !isIdentStart(S.front()))
    return StringRef();
  size_t N = 1;
  while (N < S.size() && isIdentCont(S[N]))
    ++N;
  return S.take_front(N);
}

/// True when \p S begins with \p Word followed by a non-identifier character.
bool startsWithWord(StringRef S, StringRef Word) {
  return S.starts_with(Word) &&
         (S.size() == Word.size() || !isIdentCont(S[Word.size()]));
}

/// True when \p S ends with \p Word as a whole word.
bool endsWithWord(StringRef S, StringRef Word) {
  if (!S.ends_with(Word))
    return false;
  return S.size() == Word.size() || !isIdentCont(S[S.size() - Word.size() - 1]);
}

/// Does \p T name a metavariable the rule declared as a whole statement?
bool isStatementMetaVar(StringRef T, ArrayRef<MetaVar> MetaVars) {
  T = T.trim();
  for (const MetaVar &M : MetaVars)
    if (M.Name == T && M.Kind == MetaVar::Kind::Statement)
      return true;
  return false;
}

/// The depth of unclosed `(` and `[` that \p T leaves behind, ignoring any
/// inside a string or a character literal.
///
/// A brace is deliberately not counted. `(` and `[` continue an expression
/// onto the next line, so `foo(` takes the lines after it. A `{` opens a block
/// whose contents are separate statements, and joining them produced a
/// `CompoundStmt` out of an unbalanced brace and broke the statements after it
/// in the same translation unit.
int bracketDepth(StringRef T) {
  int Depth = 0;
  bool InString = false, InChar = false;
  for (size_t I = 0, E = T.size(); I != E; ++I) {
    const char C = T[I];
    if (InString || InChar) {
      if (C == '\\' && I + 1 != E)
        ++I;
      else if ((InString && C == '"') || (InChar && C == '\''))
        InString = InChar = false;
      continue;
    }
    if (C == '"')
      InString = true;
    else if (C == '\'')
      InChar = true;
    else if (C == '(' || C == '[')
      ++Depth;
    else if (C == ')' || C == ']')
      --Depth;
  }
  return Depth;
}

/// Is every word of \p T a type keyword or a declared type metavariable?
///
/// `- long long` over `  int x;` is one declaration split by the patch, and
/// neither line is a pattern on its own. A type fragment carries no
/// declarator, so it cannot be a statement and always continues.
bool isTypeFragment(StringRef T, ArrayRef<MetaVar> MetaVars) {
  static constexpr StringRef Keywords[] = {
      "void",     "char",    "short",    "int",      "long",
      "float",    "double",  "signed",   "unsigned", "_Bool",
      "_Complex", "complex", "const",    "volatile", "restrict",
      "static",   "extern",  "register", "inline",   "auto"};
  T = T.trim();
  if (T.empty() || T.find_first_of("()[]{};,*&") != StringRef::npos)
    return false;
  bool Any = false;
  while (!T.empty()) {
    const size_t N = T.find(' ');
    const StringRef Word = T.take_front(N);
    T = N == StringRef::npos ? StringRef() : T.drop_front(N + 1).ltrim();
    if (Word.empty())
      continue;
    Any = true;
    if (llvm::is_contained(Keywords, Word))
      continue;
    const bool IsTypeVar = llvm::any_of(MetaVars, [&](const MetaVar &M) {
      return M.Kind == MetaVar::Kind::Type && M.Name == Word;
    });
    if (!IsTypeVar)
      return false;
  }
  return Any;
}

/// Does the pattern line \p T continue onto the next one?
///
/// A rule body is written a line at a time and a pattern is not, so `if (E)`
/// and its body on the next line are one pattern and the pattern parser is
/// handed fragments unless they are joined first.
///
/// A pattern that is a bare expression is NOT a continuation. Coccinelle
/// writes `- kzalloc(c * sizeof(T), E)` to remove an expression, with no
/// semicolon, and treating every line without one as a fragment refused 842
/// files.
bool continuesOntoNextLine(StringRef T, ArrayRef<MetaVar> MetaVars) {
  if (isStatementMetaVar(T, MetaVars))
    return false;
  T = T.trim();
  if (T.empty())
    return false;
  if (bracketDepth(T) > 0)
    return true;
  if (isTypeFragment(T, MetaVars))
    return true;
  StringRef R = T.rtrim();
  // A dangling `else` or `do` needs the body that follows it.
  if (endsWithWord(R, "else") || endsWithWord(R, "do"))
    return true;
  // An operator or a separator at the end has a right operand on the next
  // line. A trailing `;` or `}` ends the pattern whatever precedes it.
  if (R.ends_with(";") || R.ends_with("}"))
    return false;
  const char Last = R.back();
  if (StringRef("&|+-*/%^<>=!?:,~.").contains(Last))
    return true;
  // A statement head whose body is on the next line: the condition's closing
  // parenthesis is the last thing on the line.
  StringRef First = T;
  size_t N = 0;
  while (N != First.size() && isIdentCont(First[N]))
    ++N;
  StringRef Word = First.take_front(N);
  if (Word == "if" || Word == "while" || Word == "for" || Word == "switch")
    return R.ends_with(")");
  if (Word == "else" || Word == "do")
    return Word.size() == R.size();
  return false;
}

/// Does \p T open with an operator where an operand belongs, so that it is
/// the tail of a statement rather than the start of one?
bool opensMidStatement(StringRef T) {
  T = T.ltrim();
  return !T.empty() && StringRef("&|+*/%^<>=!?:,.)]").contains(T.front());
}

/// Does \p Next carry on from \p Above rather than start a statement?
///
/// `-static const char *str` over `    = E;` is one declaration, and the
/// backward test cannot see it: the line above ends in an identifier, which
/// is how a complete expression pattern ends too. What settles it is the line
/// below opening with an operator.
///
/// The line above having ended a statement stops this, so two independent
/// patterns are not fused when the second opens with a `*` or a `.`. So does
/// an opening brace, because joining a block's brace to the first statement
/// inside it builds unbalanced text, and every item of a rule shares one
/// translation unit, so the imbalance breaks the statements after it too.
bool continuesTheLineAbove(StringRef Above, StringRef Next) {
  Above = Above.rtrim();
  if (Above.empty() || Above.ends_with(";") || Above.ends_with("}") ||
      Above.ends_with("{"))
    return false;
  return opensMidStatement(Next);
}

/// Appends \p It to one side's statement sequence, joining it onto the
/// statement already there when that one is unfinished.
void appendToSide(std::vector<PatternItem> &Side, const PatternItem &It,
                  ArrayRef<MetaVar> MetaVars) {
  if (It.Kind != ItemKind::Statement || Side.empty() ||
      Side.back().Kind != ItemKind::Statement) {
    Side.push_back(It);
    return;
  }
  // A line holding `...` is not part of a C statement. Joining it in would
  // build an item no pattern parser can read, and it would move the item's
  // line number away from the refusal that names the ellipsis.
  if (StringRef(It.Text).contains("...") ||
      StringRef(Side.back().Text).contains("...")) {
    Side.push_back(It);
    return;
  }
  if (!continuesOntoNextLine(Side.back().Text, MetaVars) &&
      !continuesTheLineAbove(Side.back().Text, It.Text)) {
    Side.push_back(It);
    return;
  }
  Side.back().Text += " ";
  Side.back().Text += StringRef(It.Text).trim();
  // A position on a joined line still belongs to the statement.
  if (Side.back().PositionVar.empty())
    Side.back().PositionVar = It.PositionVar;
  // A statement holding one changed line is a changed statement, whichever
  // line of it opened the group.
  if (It.Marker != ItemMarker::Context)
    Side.back().Marker = It.Marker;
}

/// Fills in \p R.Minus and \p R.Plus from \p R.Body.
///
/// Grouping the two sides separately is what SmPL means by a context line. A
/// transformed statement keeps its head on a context line and its changed
/// part on a `-` or `+` line, so its lines never share one marker, and
/// grouping by same marker left 11 fewer statements of the 211-patch sample
/// parsing.
void groupSides(Rule &R) {
  for (const PatternItem &It : R.Body) {
    if (It.Marker != ItemMarker::Plus)
      appendToSide(R.Minus, It, R.MetaVars);
    if (It.Marker == ItemMarker::Context || It.Marker == ItemMarker::Plus)
      appendToSide(R.Plus, It, R.MetaVars);
  }
  // A group that closed while its text was still open never got the lines
  // that complete it, because they are on the other side. So did one that
  // opens mid-statement, as the plus side of `- static const char *str` over
  // `    = E;` does: it holds `= E;` and nothing to assign.
  for (std::vector<PatternItem> *Side : {&R.Minus, &R.Plus})
    for (PatternItem &It : *Side)
      if (It.Kind == ItemKind::Statement)
        It.Unfinished = continuesOntoNextLine(It.Text, R.MetaVars) ||
                        opensMidStatement(It.Text);
}

/// The first whitespace-separated word of \p S.
StringRef firstWord(StringRef S) {
  S = S.ltrim();
  size_t N = 0;
  while (N < S.size() && !isSpace(S[N]))
    ++N;
  return S.take_front(N);
}

/// Offset of \p Word in \p S as a whole word, skipping string and character
/// literals. StringRef::npos when absent.
size_t findWord(StringRef S, StringRef Word) {
  for (size_t I = 0, E = S.size(); I != E; ++I) {
    char C = S[I];
    if (C == '"' || C == '\'') {
      char Q = C;
      for (++I; I != E && S[I] != Q; ++I)
        if (S[I] == '\\' && I + 1 != E)
          ++I;
      continue;
    }
    if (I != 0 && isIdentCont(S[I - 1]))
      continue;
    if (startsWithWord(S.drop_front(I), Word))
      return I;
  }
  return StringRef::npos;
}

/// True when \p S holds \p Word as a whole word.
bool hasWord(StringRef S, StringRef Word) {
  return findWord(S, Word) != StringRef::npos;
}

/// True when the quote at \p I opens something short enough to be a character
/// literal. Apostrophes in prose are common in script bodies, so a lone quote
/// must not swallow the rest of the file.
bool looksLikeCharLiteral(const std::string &Buf, size_t I) {
  for (size_t J = I + 1, E = std::min(Buf.size(), I + 5); J < E; ++J) {
    if (Buf[J] == '\n')
      return false;
    if (Buf[J] == '\\') {
      ++J;
      continue;
    }
    if (Buf[J] == '\'')
      return true;
  }
  return false;
}

/// Overwrites every comment character with a space, leaving line breaks and
/// every other column in place. Column-zero markers are read after this, so a
/// '*/' closing a block comment in column 0 must not survive as a '*' marker.
/// Coccinelle's script lexer also treats '//' as a comment, so a Python body
/// is handled the same way.
void blankComments(std::string &Buf) {
  enum { Code, Block, Str, Chr } S = Code;
  for (size_t I = 0, E = Buf.size(); I != E; ++I) {
    char C = Buf[I];
    switch (S) {
    case Code:
      if (C == '/' && I + 1 != E && Buf[I + 1] == '/') {
        while (I != E && Buf[I] != '\n')
          Buf[I++] = ' ';
        --I;
      } else if (C == '/' && I + 1 != E && Buf[I + 1] == '*') {
        Buf[I] = Buf[I + 1] = ' ';
        ++I;
        S = Block;
      } else if (C == '"') {
        S = Str;
      } else if (C == '\'' && looksLikeCharLiteral(Buf, I)) {
        S = Chr;
      }
      break;
    case Block:
      if (C == '*' && I + 1 != E && Buf[I + 1] == '/') {
        Buf[I] = Buf[I + 1] = ' ';
        ++I;
        S = Code;
      } else if (C != '\n') {
        Buf[I] = ' ';
      }
      break;
    case Str:
      if (C == '\\' && I + 1 != E)
        ++I;
      else if (C == '"' || C == '\n')
        S = Code;
      break;
    case Chr:
      if (C == '\\' && I + 1 != E)
        ++I;
      else if (C == '\'' || C == '\n')
        S = Code;
      break;
    }
  }
}

/// One physical line, with its 1-based number. Column 0 of Text is column 0 of
/// the file, so leading whitespace is deliberately kept.
struct PhysLine {
  StringRef Text;
  unsigned Number = 0;
};

/// A metavariable kind phrase, and what to do with a declaration using it.
struct KindEntry {
  const char *Phrase;
  /// Null for the five kinds SemanticPatch can hold.
  const char *Construct;
  MetaKind Kind;
};

/// Longest phrase first, so that "expression list" is not read as
/// "expression" with a stray "list".
const KindEntry KindTable[] = {
    {"expression list", "expression list metavariable", MetaKind::Expression},
    {"identifier list", "identifier list metavariable", MetaKind::Identifier},
    {"parameter list", "parameter list metavariable", MetaKind::Expression},
    {"statement list", "statement list metavariable", MetaKind::Statement},
    {"field list", "field list metavariable", MetaKind::Expression},
    {"initialiser list", "initialiser list metavariable", MetaKind::Expression},
    {"initializer list", "initializer list metavariable", MetaKind::Expression},
    {"format list", "format list metavariable", MetaKind::Expression},
    {"local idexpression", "local idexpression metavariable",
     MetaKind::Expression},
    {"global idexpression", "global idexpression metavariable",
     MetaKind::Expression},
    {"idexpression", "idexpression metavariable", MetaKind::Expression},
    {"position any", "position any", MetaKind::Position},
    {"iterator name", "iterator name declaration", MetaKind::Identifier},
    {"iterator", "iterator metavariable", MetaKind::Identifier},
    {"declarer name", "declarer name declaration", MetaKind::Identifier},
    {"declarer", "declarer metavariable", MetaKind::Identifier},
    {"attribute name", "attribute name declaration", MetaKind::Identifier},
    {"attribute", "attribute metavariable", MetaKind::Identifier},
    {"binary operator", "binary operator metavariable", MetaKind::Identifier},
    {"assignment operator", "assignment operator metavariable",
     MetaKind::Identifier},
    {"operator", "operator metavariable", MetaKind::Identifier},
    {"local function", "local function metavariable", MetaKind::Identifier},
    {"function", "function metavariable", MetaKind::Identifier},
    {"fresh identifier", "fresh identifier", MetaKind::Identifier},
    {"metavariable", "metavariable (any kind)", MetaKind::Expression},
    {"pragmainfo", "pragmainfo metavariable", MetaKind::Identifier},
    {"format", "format metavariable", MetaKind::Identifier},
    {"comments", "comments metavariable", MetaKind::Identifier},
    {"error", "error metavariable", MetaKind::Expression},
    {"declaration name", "declaration name metavariable", MetaKind::Identifier},
    {"declaration", "declaration metavariable", MetaKind::Statement},
    {"initialiser", "initialiser metavariable", MetaKind::Expression},
    {"initializer", "initializer metavariable", MetaKind::Expression},
    {"parameter", "parameter metavariable", MetaKind::Expression},
    {"field", "field metavariable", MetaKind::Identifier},
    {"expression", nullptr, MetaKind::Expression},
    {"identifier", nullptr, MetaKind::Identifier},
    {"statement", nullptr, MetaKind::Statement},
    {"type", nullptr, MetaKind::Type},
    {"constant", nullptr, MetaKind::Constant},
    {"position", nullptr, MetaKind::Position},
};

/// Options a `#spatch` line may carry without changing what any pattern in
/// the file means. They select output format, verbosity, profiling,
/// parallelism or caching, so ignoring them changes nothing a reader of the
/// findings would notice.
///
/// Everything else is refused, because an embedded option that reaches the
/// front end, the include path, the isomorphisms, the integer model or the
/// control-flow graph changes how the patterns must be read, and accepting
/// the line while ignoring its content would be a silent misparse rather
/// than a gain. `--c++` is the case that matters: it selects a different
/// front end for the whole file.
const char *const InertSpatchOptions[] = {
    // output and diff format
    "-o", "-U", "--in-place", "--out-place", "--suffix", "--show-diff",
    "--no-show-diff", "--force-diff", "--keep-comments", "--linux-spacing",
    "--smpl-spacing", "--indent", "--max-width", "--patch", "--selected-only",
    // verbosity, tracing and profiling
    "--quiet", "--very-quiet", "--debug", "--pad", "--profile",
    "--profile-per-file", "--bench", "--track-iso", "--profile-iso",
    "--graphical-trace", "--gt-without-label", "--disable-once",
    "--show-trace-profile", "--print-options-only", "--parse-error-msg",
    "--type-error-msg", "--verbose-match", "--verbose-engine",
    "--verbose-ctl-engine", "--verbose-parsing", "--verbose-includes",
    "--show-trying", "--show-dependencies", "--show-bindings",
    "--show-transinfo", "--show-misc", "--show-flow", "--show-c",
    "--show-cocci", "--show-SP", "--cocci-internals", "--c-internals",
    "--debug-cpp", "--debug-lexer", "--debug-etdt", "--debug-typedef",
    "--debug-unparsing", "--debug-parse-cocci", "--filter-msg",
    "--filter-define-error", "--filter-msg-define-error",
    "--filter-passed-level",
    // parallelism, caching and temporary files
    "--jobs", "-j", "--chunksize", "--tmp-dir", "--temp-files", "--index",
    "--max", "--mod-distrib", "--use-cache", "--cache-prefix", "--cache-limit",
    "--no-include-cache", "--save-tmp-files", "--batch_mode", "--timeout",
    "--no-scanner", "--disable-worth-trying-opt"};

/// C and C++ keywords whose statement forms the subset does not enumerate.
const char *const CxxKeywords[] = {
    "template",     "namespace",        "new",       "delete", "co_return",
    "decltype",     "private",          "protected", "public", "typename",
    "constexpr",    "nullptr",          "throw",     "catch",  "static_cast",
    "dynamic_cast", "reinterpret_cast", "const_cast"};

class SmplParser {
public:
  SmplParser(StringRef Text, StringRef Filename, std::string &Error)
      : Buf(Text.str()), Filename(Filename.str()), Error(Error) {
    blankComments(Buf);
    splitLines();
  }

  std::optional<SemanticPatch> parse();

private:
  void splitLines();
  bool err(unsigned LineNo, const Twine &Msg);
  void refuse(unsigned LineNo, StringRef Construct, StringRef Reason);

  bool parsePrologueLine(const PhysLine &L);
  bool parseRule();
  bool parseCocciHeader(const PhysLine &L, StringRef H, Rule &R);
  bool parseMetaDecls(Rule &R);
  bool parseMetaDecl(unsigned LineNo, StringRef Decl, Rule &R);
  bool declareNames(unsigned LineNo, StringRef Names, MetaKind Kind,
                    bool Supported, Rule &R);
  bool parseBody(Rule &R);
  bool parseWhen(ArrayRef<PhysLine> BodyLines, size_t &Bi, StringRef WhenText,
                 PatternItem &Dots);
  bool parsePositions(const PhysLine &L, PatternItem &It, Rule &R);
  void scanRefusedConstructs(unsigned LineNo, StringRef Text, Rule &R,
                             bool BodyFollows);
  bool parseScriptRule(const PhysLine &L, StringRef H);
  void skipRuleTail();
  bool setFileMode(const PhysLine &L, bool IsMatch);
  bool checkDependency(const PhysLine &L, ArrayRef<StringRef> Dep, Rule &R);

  /// "rule.name" for the inheritance tables.
  static std::string qualify(StringRef Rule, StringRef Name) {
    return (Rule + "." + Name).str();
  }

  std::string Buf;
  std::string Filename;
  std::string &Error;
  SmallVector<PhysLine, 64> Lines;
  size_t Pos = 0;

  SemanticPatch Patch;
  StringSet<> RuleNames;
  /// Names introduced by a refused `virtual` declaration. A `depends on patch`
  /// resolves against these, so refusing `virtual` does not cascade into a
  /// bogus unknown-dependency error.
  StringSet<> VirtualNames;
  StringMap<MetaKind> MetaVarKinds;
  StringSet<> RefusedMetaVars;
  /// Every metavariable name seen anywhere in the file. A name declared in one
  /// rule and used unqualified in another is a literal C identifier to
  /// Coccinelle, which warns and matches nothing useful.
  StringSet<> AllMetaVarNames;
  StringMap<MetaKind> CurDecls;
  StringSet<> CurRefused;
  /// Names this rule declared with `symbol`, which are literal C text here.
  StringSet<> CurLiterals;
  /// Names declared with `typedef`, which are type names for the whole file
  /// the way Coccinelle's own type table treats them.
  StringSet<> TypeNames;
  StringSet<> SeenRefusals;

  /// Patch mode ('-'/'+') and match mode ('*') are file-global and exclusive.
  enum class Mode { Unknown, Patch, Match } FileMode = Mode::Unknown;
  /// Set by a refused file-level #include, after which a name this file never
  /// declares may come from the included file rather than be undefined.
  bool HasRefusedInclude = false;
  /// True while parsing a rule whose dependency names a virtual rule.
  /// Coccinelle drops such a rule before fixing the file's marker mode when
  /// the virtual is not given with -D, which is how a kernel patch can carry
  /// a '-'/'+' variant and a '*' variant of the same rule in one file.
  bool CurRuleVirtualGuarded = false;
  /// Set once any rule modifies the tree. Coccinelle clears its inheritable
  /// position list at that point, so a later `position r.p` is refused.
  bool ModificationSeen = false;
};

void SmplParser::splitLines() {
  StringRef Rest(Buf);
  unsigned N = 1;
  while (true) {
    auto Split = Rest.split('\n');
    Lines.push_back({Split.first.rtrim('\r'), N++});
    if (Split.second.data() == nullptr)
      break;
    Rest = Split.second;
  }
}

bool SmplParser::err(unsigned LineNo, const Twine &Msg) {
  Error = (Filename + ":" + Twine(LineNo) + ": " + Msg).str();
  return false;
}

void SmplParser::refuse(unsigned LineNo, StringRef Construct,
                        StringRef Reason) {
  std::string Key = (Twine(LineNo) + "|" + Construct).str();
  if (!SeenRefusals.insert(Key).second)
    return;
  Patch.Refusals.push_back({Construct.str(), Reason.str(), LineNo});
}

/// Splits a rule header into identifiers, numbers, string literals, "&&",
/// "||" and single punctuation characters.
void tokenizeHeader(StringRef H, SmallVectorImpl<StringRef> &Toks) {
  size_t I = 0;
  while (I < H.size()) {
    char C = H[I];
    if (isSpace(C)) {
      ++I;
      continue;
    }
    if (isIdentStart(C)) {
      StringRef Id = takeIdent(H.drop_front(I));
      Toks.push_back(Id);
      I += Id.size();
      continue;
    }
    if (isDigit(C)) {
      size_t N = I;
      while (N < H.size() && isAlnum(H[N]))
        ++N;
      Toks.push_back(H.slice(I, N));
      I = N;
      continue;
    }
    if (C == '"') {
      size_t N = I + 1;
      while (N < H.size() && H[N] != '"')
        ++N;
      if (N < H.size())
        ++N;
      Toks.push_back(H.slice(I, N));
      I = N;
      continue;
    }
    if ((C == '&' || C == '|') && I + 1 < H.size() && H[I + 1] == C) {
      Toks.push_back(H.slice(I, I + 2));
      I += 2;
      continue;
    }
    Toks.push_back(H.slice(I, I + 1));
    ++I;
  }
}

bool isHeaderKeyword(StringRef T) {
  return StringSwitch<bool>(T)
      .Cases({"depends", "on", "exists", "forall"}, true)
      .Cases({"extends", "using", "disable", "generated"}, true)
      .Cases({"expression", "identifier", "type"}, true)
      .Cases({"script", "initialize", "finalize"}, true)
      .Default(false);
}

std::optional<SemanticPatch> SmplParser::parse() {
  bool SawRule = false;
  while (Pos < Lines.size()) {
    const PhysLine L = Lines[Pos];
    StringRef T = L.Text.trim();
    if (T.empty()) {
      ++Pos;
      continue;
    }
    if (T.front() == '@') {
      // '@@' is a single lexer token and is recognised at any indentation. A
      // header carrying a name, a dependency or a quantifier is not.
      if (L.Text.front() != '@' && T != "@@") {
        err(L.Number, "a rule header carrying options must start at column 0");
        return std::nullopt;
      }
      if (!parseRule())
        return std::nullopt;
      SawRule = true;
      continue;
    }
    if (SawRule) {
      err(L.Number, "expected a rule header; a rule body ends at the '@' that "
                    "opens the next header");
      return std::nullopt;
    }
    if (!parsePrologueLine(L))
      return std::nullopt;
    ++Pos;
  }
  if (!SawRule) {
    Error = (Twine(Filename) +
             ": no rule header found, so this is not a semantic patch")
                .str();
    return std::nullopt;
  }
  return std::move(Patch);
}

bool SmplParser::parsePrologueLine(const PhysLine &L) {
  StringRef T = L.Text.trim();
  if (startsWithWord(T, "virtual")) {
    StringRef Names = T.drop_front(StringRef("virtual").size());
    while (!Names.trim().empty()) {
      auto Split = Names.trim().split(',');
      StringRef Name = takeIdent(Split.first.trim());
      if (Name.empty())
        return err(L.Number, "expected a rule name after 'virtual'");
      VirtualNames.insert(Name);
      Patch.Virtuals.push_back(Name.str());
      Names = Split.second;
    }
    return true;
  }
  if (startsWithWord(T, "using")) {
    refuse(L.Number, "using \"...\" isomorphism file",
           "an isomorphism file rewrites terms silently during matching, so "
           "the pattern applied would not be the pattern written");
    return true;
  }
  if (T.front() == '#' && T.drop_front().ltrim().starts_with("spatch")) {
    StringRef Rest = T.drop_front().ltrim().drop_front(strlen("spatch"));
    SmallVector<StringRef, 8> Words;
    while (true) {
      Rest = Rest.ltrim();
      if (Rest.empty())
        break;
      size_t N = 0;
      while (N != Rest.size() && !isSpace(Rest[N]))
        ++N;
      Words.push_back(Rest.take_front(N));
      Rest = Rest.drop_front(N);
    }
    for (size_t I = 0; I != Words.size(); ++I) {
      StringRef Opt = Words[I];
      if (!Opt.starts_with("-"))
        continue; // the value of the option before it
      if (Opt.starts_with("--c++")) {
        refuse(L.Number, "#spatch --c++",
               "the option selects Coccinelle's C++ front end for the whole "
               "file, so every pattern in it is read differently");
        continue;
      }
      bool Inert = false;
      for (const char *K : InertSpatchOptions)
        if (Opt == K)
          Inert = true;
      if (!Inert)
        refuse(L.Number, "#spatch embedded options",
               ("the option " + Opt +
                " changes how the patch is read or which code it sees, and "
                "this tool takes that from its own command line instead")
                   .str());
    }
    return true;
  }
  if (T.starts_with("#include")) {
    HasRefusedInclude = true;
    refuse(L.Number, "file-level #include",
           "a prologue #include declares types for the whole file, which this "
           "tool takes from the compilation database instead");
    return true;
  }
  if (T.starts_with("#define") || T.starts_with("#undef") ||
      T.starts_with("#pragma"))
    return err(L.Number, "a preprocessor directive cannot appear before the "
                         "first rule header");
  return err(L.Number, "stray text before the first rule header");
}

void SmplParser::skipRuleTail() {
  // Metavariable declarations up to the closing '@@', then the body up to the
  // '@' that opens the next header.
  while (Pos < Lines.size()) {
    StringRef T = Lines[Pos].Text.trim();
    ++Pos;
    if (T.starts_with("@@"))
      break;
  }
  while (Pos < Lines.size()) {
    StringRef Text = Lines[Pos].Text;
    if (!Text.empty() && Text.front() == '@')
      break;
    if (Text.trim() == "@@")
      break;
    ++Pos;
  }
}

bool SmplParser::parseRule() {
  const PhysLine HL = Lines[Pos];
  StringRef T = HL.Text.trim();
  std::string HeaderText;
  bool DeclsOnHeaderLine = false;
  if (T != "@@") {
    StringRef Rest = HL.Text.drop_front();
    size_t Close = Rest.find('@');
    // A header may be laid out over several lines, Coccinelle lexing header
    // tokens until the closing '@'. The search is bounded so that a stray
    // column-zero '@' in a body is reported where it is rather than
    // swallowing the rest of the file.
    size_t Extra = 0;
    while (Close == StringRef::npos) {
      HeaderText += Rest.str();
      HeaderText += ' ';
      ++Extra;
      if (Extra > 8 || Pos + Extra >= Lines.size())
        return err(HL.Number,
                   "unterminated rule header: no closing '@'. A '@' first on "
                   "a line opens a header, so a position variable cannot "
                   "start a line");
      Rest = Lines[Pos + Extra].Text;
      Close = Rest.find('@');
    }
    Pos += Extra;
    HeaderText += Rest.take_front(Close).str();
    // The declaration block may start on the header's own line, as in the
    // one-line `@@ @@` form. What follows the closing '@' is handed to the
    // declaration parser instead of being read as part of the header.
    StringRef Trailing = Rest.drop_front(Close + 1);
    if (!Trailing.trim().empty()) {
      Lines[Pos].Text = Trailing;
      DeclsOnHeaderLine = true;
    }
  }
  if (!DeclsOnHeaderLine)
    ++Pos;
  StringRef H = StringRef(HeaderText).trim();

  bool IsScript = H.starts_with("script") &&
                  (H.size() == 6 || H[6] == ':' || isSpace(H[6]));
  if (IsScript)
    return parseScriptRule(HL, H);

  bool IsInit = H.starts_with("initialize");
  bool IsFini = H.starts_with("finalize");
  if (IsInit || IsFini) {
    StringRef Lang = takeIdent(H.split(':').second.trim());
    if (Lang.empty())
      Lang = "python";
    std::string Construct =
        Lang == "ocaml" ? "@initialize:ocaml@ / @finalize:ocaml@ rule"
                        : "@initialize:python@ / @finalize:python@ rule";
    refuse(HL.Number, Construct,
           "code that runs once before or after the whole run has no "
           "counterpart in a single-pattern match");
    skipRuleTail();
    return true;
  }

  Rule R;
  R.Line = HL.Number;
  CurDecls.clear();
  CurRefused.clear();
  CurLiterals.clear();
  CurRuleVirtualGuarded = false;
  if (!parseCocciHeader(HL, H, R))
    return false;
  if (!parseMetaDecls(R))
    return false;
  if (!parseBody(R))
    return false;
  // A fragment that survives grouping is reported by the pattern parser,
  // which knows whether it parses. A second heuristic check here refused 70
  // patches that were fine.
  groupSides(R);
  Patch.Rules.push_back(std::move(R));
  return true;
}

bool SmplParser::checkDependency(const PhysLine &L, ArrayRef<StringRef> Dep,
                                 Rule &R) {
  if (Dep.empty())
    return err(L.Number, "'depends on' with no dependency");

  bool Boolean = false, Negated = false, FileIn = false, EverNever = false;
  bool Quantified = false, NamesARule = false;
  // Every token is a slice of the header, so the expression can be kept as
  // it was written.
  StringRef Text(Dep.front().data(), Dep.back().end() - Dep.front().begin());
  for (size_t I = 0; I != Dep.size(); ++I) {
    StringRef D = Dep[I];
    if (VirtualNames.count(D))
      CurRuleVirtualGuarded = true;
    if (D == "&&" || D == "||")
      Boolean = true;
    else if (D == "!")
      Negated = true;
    else if (D == "file" && I + 1 != Dep.size() && Dep[I + 1] == "in")
      FileIn = true;
    else if (D == "ever" || D == "never")
      EverNever = true;
    else if (D == "exists" || D == "forall")
      Quantified = true;
    else if (D == "in" || D == "(" || D == ")" || D.starts_with("\""))
      continue;
    else if (!takeIdent(D).empty() && !VirtualNames.count(D)) {
      if (!RuleNames.count(D)) {
        if (!HasRefusedInclude)
          return err(L.Number, "'depends on " + D +
                                   "' names a rule that no earlier line "
                                   "declares");
        refuse(L.Number, "depends on a rule in an included .cocci file",
               "the rule it names is declared in a file this tool does not "
               "read, so the dependency cannot be checked");
        return true;
      }
      NamesARule = true;
    }
  }

  if (FileIn)
    refuse(L.Number, "depends on file in",
           "restricting a rule to one file path has no counterpart in a "
           "compilation-database driven tool");
  if (EverNever)
    refuse(L.Number, "depends on ever / never",
           "the dependency asks whether the other rule matched anywhere in "
           "the whole run, which is not a property of one match");
  if (Quantified)
    refuse(L.Number, "depends on exists / forall",
           "the dependency quantifies over environments, which this tool "
           "does not model");
  // A boolean over virtual rules alone selects the output mode and is inside
  // the subset. Mixing a rule name into it makes the dependency a per-match
  // property, which is a different construct.
  if (NamesARule && Negated)
    refuse(L.Number, "depends on negated rule",
           "the dependency has to be evaluated per match, and it is not "
           "recorded, so this rule must not be applied on its own");
  if (NamesARule && Boolean)
    refuse(L.Number, "depends on boolean expression over a rule name",
           "the dependency has to be evaluated per match, and it is not "
           "recorded, so this rule must not be applied on its own");

  R.DependsOnVirtual = !NamesARule;
  if (Dep.size() == 1 && !takeIdent(Dep[0]).empty()) {
    R.DependsOn = Dep[0].str();
    return true;
  }
  // The expression is kept verbatim rather than dropped, so a consumer sees
  // that the rule is conditional even when the condition is not a single
  // name.
  if (!NamesARule)
    R.DependsOn = Text.str();
  return true;
}

bool SmplParser::parseCocciHeader(const PhysLine &L, StringRef H, Rule &R) {
  size_t DependsAt = findWord(H, "depends");
  for (StringRef Q : {"exists", "forall"}) {
    size_t QAt = findWord(H, Q);
    if (QAt != StringRef::npos && DependsAt != StringRef::npos &&
        QAt < DependsAt)
      return err(L.Number, "'depends on' must come before the rule "
                           "quantifier; Coccinelle rejects the other order");
  }

  SmallVector<StringRef, 8> Toks;
  tokenizeHeader(H, Toks);
  size_t I = 0;
  if (I != Toks.size() && !takeIdent(Toks[I]).empty() &&
      !isHeaderKeyword(Toks[I])) {
    R.Name = Toks[I].str();
    if (!RuleNames.insert(R.Name).second)
      return err(L.Number, "duplicate rule name '" + R.Name + "'");
    ++I;
  }

  bool SawQuant = false;
  while (I != Toks.size()) {
    StringRef Tok = Toks[I];
    if (Tok == "depends") {
      ++I;
      if (I == Toks.size() || Toks[I] != "on")
        return err(L.Number, "expected 'on' after 'depends'");
      ++I;
      // The dependency expression ends at the next header option, so that
      // `depends on r disable iso` does not read `disable` as a rule name.
      size_t End = I;
      while (End != Toks.size()) {
        StringRef Tok = Toks[End];
        if (Tok == "disable" || Tok == "using" || Tok == "extends" ||
            Tok == "generated" || Tok == "expression" || Tok == "identifier" ||
            Tok == "type")
          break;
        ++End;
      }
      if (End > I + 1 &&
          (Toks[End - 1] == "exists" || Toks[End - 1] == "forall"))
        --End;
      if (!checkDependency(L, ArrayRef<StringRef>(Toks).slice(I, End - I), R))
        return false;
      I = End;
      continue;
    }
    if (Tok == "exists" || Tok == "forall") {
      R.Quant =
          Tok == "exists" ? Rule::Quantifier::Exists : Rule::Quantifier::Forall;
      SawQuant = true;
      ++I;
      continue;
    }
    if (Tok == "extends") {
      refuse(L.Number, "extends",
             "importing every metavariable of another rule without naming "
             "them leaves the pattern's variables undetermined");
      ++I;
      if (I != Toks.size())
        ++I;
      continue;
    }
    if (Tok == "using") {
      refuse(L.Number, "using \"...\" isomorphism file",
             "an isomorphism file rewrites terms silently during matching, so "
             "the pattern applied would not be the pattern written");
      ++I;
      if (I != Toks.size())
        ++I;
      continue;
    }
    if (Tok == "disable") {
      refuse(L.Number, "disable <isomorphism>",
             "the built-in isomorphisms it turns off are not implemented, so "
             "turning one off cannot be honoured");
      ++I;
      while (I != Toks.size() &&
             (Toks[I] == "," || !takeIdent(Toks[I]).empty()))
        ++I;
      continue;
    }
    if (Tok == "generated") {
      refuse(L.Number, "@generated@ rule",
             "a generated rule is produced by --hrule, which this tool does "
             "not implement");
      ++I;
      continue;
    }
    if (Tok == "expression" || Tok == "identifier" || Tok == "type") {
      refuse(L.Number, "expression / identifier / type rule kind",
             "forcing the whole pattern to parse as one syntactic category is "
             "not supported");
      ++I;
      continue;
    }
    return err(L.Number, "unexpected '" + Tok + "' in rule header");
  }
  (void)SawQuant;
  return true;
}

/// Runs of whitespace become one space, so that a two-word kind phrase is
/// recognised however it was laid out.
std::string collapseSpaces(StringRef S) {
  std::string Out;
  bool Space = false;
  for (char C : S) {
    if (isSpace(C)) {
      Space = true;
      continue;
    }
    if (Space && !Out.empty())
      Out += ' ';
    Space = false;
    Out += C;
  }
  return Out;
}

/// The trailing identifier of \p S, which is the declared name in both
/// `E` and `INTEGRAL *BYTES`.
StringRef lastIdent(StringRef S) {
  S = S.rtrim();
  while (!S.empty() && !isIdentCont(S.back()))
    S = S.drop_back();
  size_t N = S.size();
  while (N != 0 && isIdentCont(S[N - 1]))
    --N;
  return S.drop_front(N);
}

/// Appends the pieces of \p S separated by top-level \p Sep, skipping
/// separators nested in brackets or in a string.
void splitTopLevel(StringRef S, char Sep, SmallVectorImpl<StringRef> &Out) {
  int Depth = 0;
  size_t Start = 0;
  for (size_t I = 0, E = S.size(); I != E; ++I) {
    char C = S[I];
    if (C == '"' || C == '\'') {
      char Q = C;
      for (++I; I != E && S[I] != Q; ++I)
        if (S[I] == '\\' && I + 1 != E)
          ++I;
      continue;
    }
    if (C == '(' || C == '[' || C == '{')
      ++Depth;
    else if (C == ')' || C == ']' || C == '}')
      --Depth;
    else if (C == Sep && Depth <= 0) {
      Out.push_back(S.slice(Start, I));
      Start = I + 1;
    }
  }
  Out.push_back(S.drop_front(Start));
}

/// Offset of the first top-level constraint operator in a declaration's name
/// list, which is where the names stop and a constraint begins.
size_t findConstraint(StringRef S) {
  int Depth = 0;
  for (size_t I = 0, E = S.size(); I != E; ++I) {
    char C = S[I];
    if (C == '"' || C == '\'') {
      char Q = C;
      for (++I; I != E && S[I] != Q; ++I)
        if (S[I] == '\\' && I + 1 != E)
          ++I;
      continue;
    }
    if (C == '(' || C == '[' || C == '{')
      ++Depth;
    else if (C == ')' || C == ']' || C == '}')
      --Depth;
    else if (Depth <= 0 && (C == '=' || C == ':' || C == '~' || C == '!'))
      return I;
  }
  return StringRef::npos;
}

/// Drops a trailing `[...]`, the fixed-length form of a list metavariable.
StringRef stripTrailingBracket(StringRef S) {
  S = S.rtrim();
  if (S.empty() || S.back() != ']')
    return S;
  size_t Open = S.rfind('[');
  return Open == StringRef::npos ? S : S.take_front(Open);
}

bool SmplParser::declareNames(unsigned LineNo, StringRef Names, MetaKind Kind,
                              bool Supported, Rule &R) {
  SmallVector<StringRef, 8> Chunks;
  splitTopLevel(Names, ',', Chunks);
  for (StringRef Chunk : Chunks) {
    // Each name carries its own constraint, as in
    // `position free.p1!=loop.ok,p2!={print.p,sz.p};`.
    size_t Constraint = findConstraint(Chunk);
    if (Constraint != StringRef::npos)
      Chunk = Chunk.take_front(Constraint);
    Chunk = Chunk.trim();
    // A leading `[n]` is a list metavariable's length, not a name.
    while (Chunk.starts_with("[")) {
      size_t Close = Chunk.find(']');
      if (Close == StringRef::npos)
        break;
      Chunk = Chunk.drop_front(Close + 1).trim();
    }
    Chunk = stripTrailingBracket(Chunk).trim();
    if (Chunk.empty())
      continue;

    bool Sup = Supported;
    std::string Owner;
    size_t Dot = Chunk.find('.');
    if (Dot != StringRef::npos) {
      StringRef Cand = Chunk.take_front(Dot).trim();
      if (!Cand.empty() && takeIdent(Cand) == Cand) {
        Owner = Cand.str();
        Chunk = Chunk.drop_front(Dot + 1);
      }
    }
    StringRef Name = lastIdent(Chunk);
    if (Name.empty())
      return err(LineNo, "expected a metavariable name");

    if (CurDecls.count(Name) || CurRefused.count(Name))
      return err(LineNo, "metavariable '" + Name +
                             "' is declared twice in the same rule");

    if (Owner == "virtual" || Owner == "merge") {
      refuse(LineNo,
             Owner == "virtual" ? "virtual.x command-line metavariable value"
                                : "script merge variable",
             "the value comes from the command line or from a whole-run merge "
             "variable rather than from the pattern");
      Owner.clear();
      Sup = false;
    } else if (!Owner.empty()) {
      std::string Qual = qualify(Owner, Name);
      if (!RuleNames.count(Owner)) {
        if (!HasRefusedInclude)
          return err(LineNo, "'" + Owner + "." + Name +
                                 "' names a rule that is not declared "
                                 "earlier in the file");
        refuse(LineNo, "inheritance from a rule in an included .cocci file",
               "the rule it names is declared in a file this tool does not "
               "read, so nothing is known about the variable");
        Owner.clear();
        Sup = false;
      } else if (Owner.empty()) {
        // handled above
      } else if (RefusedMetaVars.count(Qual)) {
        refuse(LineNo, "inheritance from a refused metavariable",
               "the declaration in the earlier rule was itself refused, so "
               "nothing is known about this variable");
        Sup = false;
      } else {
        auto It = MetaVarKinds.find(Qual);
        if (It == MetaVarKinds.end())
          return err(LineNo, "rule '" + Owner + "' declares no metavariable '" +
                                 Name + "'");
        if (Sup && It->second != Kind)
          return err(LineNo, "incompatible inheritance declaration for '" +
                                 Name +
                                 "': the earlier rule gave it another "
                                 "kind");
      }
      // Coccinelle clears its inheritable-position list as soon as any rule
      // modifies the tree, so this fails even when the modifying rule is
      // unrelated to either end of the inheritance.
      if (Kind == MetaKind::Position && ModificationSeen)
        return err(LineNo, "a position cannot be inherited across a rule that "
                           "modifies the tree: '" +
                               Name + "'");
    }

    AllMetaVarNames.insert(Name);
    if (Sup) {
      CurDecls[Name] = Kind;
      MetaVar MV;
      MV.Kind = Kind;
      MV.Name = Name.str();
      MV.InheritedFrom = Owner;
      R.MetaVars.push_back(std::move(MV));
      if (!R.Name.empty())
        MetaVarKinds[qualify(R.Name, Name)] = Kind;
    } else {
      CurRefused.insert(Name);
      if (!R.Name.empty())
        RefusedMetaVars.insert(qualify(R.Name, Name));
    }
  }
  return true;
}

bool SmplParser::parseMetaDecl(unsigned LineNo, StringRef Decl, Rule &R) {
  std::string NormStr = collapseSpaces(Decl);
  StringRef D(NormStr);
  if (D.empty())
    return true;
  if (D.front() == '?' || D.front() == '+')
    return err(LineNo, "'?' or '+' arity prefix on a metavariable "
                       "declaration: Coccinelle's lexer rejects it");

  if (D.contains("[]") || D.contains("[ ]"))
    refuse(LineNo, "array-typed metavariable declaration",
           "an array-typed declaration constrains the metavariable to a shape "
           "this tool does not compare");
  if (D.contains("<="))
    refuse(LineNo, "metavariable bound to a subterm of an inherited one (<=)",
           "the binding is decided by a subterm relation computed at match "
           "time");

  // `symbol x;` says the name is literal C text rather than a metavariable,
  // and an undeclared name is literal text here already, so the declaration
  // needs nothing recorded. `typedef t;` says the name is a type name, which
  // Coccinelle needs because its own C parser has no other way to know, and
  // which this tool takes from the translation unit. Both are inert: the
  // compiled pattern is the same with them and without them.
  bool IsSymbol = startsWithWord(D, "symbol");
  bool IsTypedef = startsWithWord(D, "typedef");
  if (IsSymbol || IsTypedef) {
    StringRef Names =
        D.drop_front(IsSymbol ? strlen("symbol") : strlen("typedef"));
    if (findConstraint(Names) != StringRef::npos) {
      refuse(LineNo, IsSymbol ? "symbol declaration" : "typedef declaration",
             "a constraint on the declaration makes it more than a statement "
             "about how the name is read");
      return true;
    }
    SmallVector<StringRef, 8> Chunks;
    splitTopLevel(Names, ',', Chunks);
    for (StringRef Chunk : Chunks) {
      StringRef Name = lastIdent(Chunk.trim());
      if (Name.empty())
        return err(LineNo, "expected a name after '" +
                               StringRef(IsSymbol ? "symbol" : "typedef") +
                               "'");
      if (IsSymbol)
        CurLiterals.insert(Name);
      else
        TypeNames.insert(Name);
    }
    return true;
  }

  const KindEntry *KE = nullptr;
  StringRef Rest;
  for (const KindEntry &E : KindTable) {
    if (startsWithWord(D, E.Phrase)) {
      KE = &E;
      Rest = D.drop_front(std::strlen(E.Phrase));
      break;
    }
  }

  bool HasBrace = D.contains('{');
  bool HasRegex = D.contains("=~") || D.contains("!~");
  bool HasScript = D.contains("script:");
  bool HasConcat = D.contains("##");
  bool IsFresh = KE && StringRef(KE->Phrase) == "fresh identifier";

  if (!KE) {
    StringRef Names;
    if (D.front() == '{') {
      size_t Close = D.find('}');
      if (Close == StringRef::npos)
        return err(LineNo, "unterminated '{' in a metavariable declaration");
      refuse(LineNo, "set-valued metavariable constraint",
             "one declaration standing for a list of types has no "
             "single-value counterpart in SemanticPatch");
      Names = D.drop_front(Close + 1);
    } else if (D.find(' ') == StringRef::npos) {
      return err(LineNo, "unrecognised metavariable declaration '" + D + "'");
    } else {
      StringRef Head = firstWord(D);
      if (AllMetaVarNames.count(Head) || CurDecls.count(Head) ||
          CurRefused.count(Head))
        refuse(LineNo, "metavariable typed by another metavariable",
               "the type comes from a second metavariable, so the type "
               "constraint is only known once that one is bound");
      else
        refuse(LineNo, "metavariable restricted to a concrete C type",
               "restricting a metavariable to a written C type needs type "
               "comparison this tool does not do");
      Names = D.drop_front(Head.size());
    }
    return declareNames(LineNo, Names, MetaKind::Expression,
                        /*Supported=*/false, R);
  }

  if (HasRegex)
    refuse(LineNo, "regex constraint on a metavariable",
           "the pattern would match a set of names decided by a regular "
           "expression, which is not part of the pattern language here");
  if (HasBrace)
    refuse(LineNo, "set-valued metavariable constraint",
           "one declaration standing for a list of alternatives has no "
           "single-value counterpart in SemanticPatch");
  if (IsFresh && HasConcat)
    refuse(LineNo, "fresh identifier with ## concatenation",
           "the replacement name is built by pasting text at match time, so "
           "it is not known while the pattern is compiled");
  if (HasScript)
    refuse(LineNo,
           D.contains("script:ocaml") ? "ocaml constraint on a metavariable"
                                      : "python constraint on a metavariable",
           "the constraint is a script predicate evaluated at match time");
  // A position with an excluded position set (`p != {r1.p, r2.p}`) is the
  // ordinary way to suppress a match, and the set form is refused above.
  bool IsPosition = StringRef(KE->Phrase) == "position";
  if (!HasRegex && !HasBrace && !HasScript && !IsFresh && !IsPosition &&
      D.contains('='))
    refuse(LineNo, "value constraint on a metavariable",
           "the fixed or excluded value is not recorded, so the metavariable "
           "would match more than the patch asks for");
  if (!HasScript && D.contains(':'))
    refuse(LineNo, "type-restricted metavariable declaration",
           "restricting a metavariable to a written C type needs type "
           "comparison this tool does not do");

  if (KE->Construct)
    refuse(LineNo, KE->Construct,
           "this metavariable kind is outside the six kinds the subset "
           "covers");
  return declareNames(LineNo, Rest, KE->Kind, /*Supported=*/!KE->Construct, R);
}

bool SmplParser::parseMetaDecls(Rule &R) {
  std::string Acc;
  unsigned AccLine = 0;
  auto Flush = [&](unsigned LineNo) -> bool {
    size_t Semi;
    while ((Semi = StringRef(Acc).find(';')) != StringRef::npos) {
      std::string One = Acc.substr(0, Semi);
      Acc.erase(0, Semi + 1);
      if (!parseMetaDecl(LineNo, One, R))
        return false;
    }
    if (StringRef(Acc).trim().empty())
      Acc.clear();
    return true;
  };

  while (Pos < Lines.size()) {
    const PhysLine L = Lines[Pos];
    StringRef T = L.Text.trim();
    if (Acc.empty()) {
      if (T.empty()) {
        ++Pos;
        continue;
      }
      if (T.front() == '@' && !T.starts_with("@@"))
        return err(L.Number, "the metavariable block must be closed by '@@', "
                             "not by a single '@'");
      AccLine = L.Number;
    }
    // The block closes at the first '@@', which may sit on the same line as
    // the last declaration, as in the one-line `@@ statement s; @@` form.
    size_t Close = T.find("@@");
    if (Close != StringRef::npos) {
      if (!Acc.empty())
        Acc += ' ';
      Acc += T.take_front(Close).str();
      if (!Flush(AccLine))
        return false;
      if (!StringRef(Acc).trim().empty())
        return err(L.Number, "a metavariable declaration is missing its ';' "
                             "before the closing '@@'");
      StringRef After = T.drop_front(Close + 2);
      if (After.trim().empty())
        ++Pos;
      else
        Lines[Pos].Text = After;
      return true;
    }
    if (!Acc.empty())
      Acc += ' ';
    Acc += T.str();
    ++Pos;
    if (!Flush(AccLine))
      return false;
  }
  return err(Lines.empty() ? 1 : Lines.back().Number,
             "unterminated metavariable block: expected '@@'");
}

/// True when \p T has the shape of a function definition or prototype
/// pattern rather than a statement. The text before the first '(' has to read
/// as a declarator, that is identifiers, '*' and whitespace only, so that an
/// expression such as `x ? y : getenv("D")` is not mistaken for one.
bool looksLikeFunctionHeader(StringRef T) {
  size_t Paren = T.find('(');
  if (Paren == StringRef::npos)
    return false;
  StringRef Head = T.take_front(Paren).trim();
  if (Head.empty())
    return false;
  for (char C : Head)
    if (!isIdentCont(C) && C != '*' && !isSpace(C))
      return false;
  SmallVector<StringRef, 4> Words;
  while (true) {
    Head = Head.ltrim();
    if (Head.empty())
      break;
    size_t N = 0;
    while (N != Head.size() && !isSpace(Head[N]))
      ++N;
    Words.push_back(Head.take_front(N));
    Head = Head.drop_front(N);
  }
  StringRef First = Words.front();
  if (First == "if" || First == "while" || First == "for" ||
      First == "switch" || First == "return" || First == "do" ||
      First == "else" || First == "sizeof" || First == "typedef")
    return false;
  // A return type plus a name, or a bare name whose body opens on this line.
  return Words.size() >= 2 || T.rtrim().ends_with("{");
}

bool SmplParser::setFileMode(const PhysLine &L, bool IsMatch) {
  Mode M = IsMatch ? Mode::Match : Mode::Patch;
  if (FileMode == Mode::Unknown) {
    FileMode = M;
    return true;
  }
  if (FileMode != M)
    return err(L.Number, "a file may use '-'/'+' or '*', never both: patch "
                         "mode and match mode are file-global");
  return true;
}

/// Characters that make a neighbouring "..." an expression-level ellipsis.
bool isOpChar(char C) { return StringRef("&|+-*/%^<>=!?:~").contains(C); }

/// True when \p S reads as the head of a record body, that is names and '*'
/// only, as in `typedef T1 {` or `struct_name {`. A brace after that opens a
/// field list rather than a block.
bool isRecordHeader(StringRef S) {
  S = S.trim();
  if (S.empty())
    return false;
  for (char C : S)
    if (!isIdentCont(C) && C != '*' && !isSpace(C))
      return false;
  StringRef First = firstWord(S);
  return First != "else" && First != "do";
}

/// What the bracket enclosing a "..." belongs to.
///
/// The level decides whether an ellipsis is a path through the control-flow
/// graph or a list of terms, and the levels are not interchangeable: a call's
/// argument list, a parameter list, a condition and an attribute argument list
/// all reach the parser as "..." between parentheses.
enum class DotsOwner {
  Call,
  Parameter,
  Condition,
  Attribute,
  Record,
  Enum,
  Initialiser,
  Array,
  Block,
  Statement
};

/// The bracket enclosing the "..." at \p At, and what it belongs to.
///
/// Returns Statement when no bracket on this line encloses the position, and
/// \p Closed reports whether that bracket also closes on this line. A bracket
/// left open cannot be classified by shape, so a caller must not read a shape
/// out of it.
DotsOwner dotsOwner(StringRef T, size_t At, bool BodyFollows, bool &Closed) {
  Closed = false;
  int Depth = 0;
  size_t Owner = StringRef::npos;
  for (size_t I = At; I-- > 0;) {
    const char C = T[I];
    if (C == ')' || C == ']' || C == '}')
      ++Depth;
    else if (C == '(' || C == '[' || C == '{') {
      if (Depth == 0) {
        Owner = I;
        break;
      }
      --Depth;
    }
  }
  if (Owner == StringRef::npos)
    return DotsOwner::Statement;

  StringRef Head = T.take_front(Owner).rtrim();
  const char Open = T[Owner];

  // Find the matching close, so a caller can tell a complete argument list
  // from one continued on the next line.
  Depth = 0;
  size_t Close = StringRef::npos;
  for (size_t I = Owner, E = T.size(); I != E; ++I) {
    if (T[I] == '(' || T[I] == '[' || T[I] == '{')
      ++Depth;
    else if (T[I] == ')' || T[I] == ']' || T[I] == '}') {
      if (--Depth == 0) {
        Close = I;
        break;
      }
    }
  }
  Closed = Close != StringRef::npos;

  if (Open == '[')
    return DotsOwner::Array;
  if (Open == '{') {
    if (Head.ends_with("="))
      return DotsOwner::Initialiser;
    if (hasWord(Head, "enum"))
      return DotsOwner::Enum;
    if (hasWord(Head, "struct") || hasWord(Head, "union") ||
        isRecordHeader(Head))
      return DotsOwner::Record;
    return DotsOwner::Block;
  }

  // An open parenthesis. What precedes it decides, except that a nested
  // parenthesis inside `__attribute__((...))` has no name before it.
  if (Head.ends_with("("))
    Head = Head.drop_back(1).rtrim();
  size_t NameAt = Head.size();
  while (NameAt != 0 && isIdentCont(Head[NameAt - 1]))
    --NameAt;
  const StringRef Name = Head.drop_front(NameAt);
  if (Name == "if" || Name == "while" || Name == "for" || Name == "switch")
    return DotsOwner::Condition;
  if (Name == "__attribute__")
    return DotsOwner::Attribute;
  // A body after the list makes it a definition's parameter list. Coccinelle
  // writes a bodyless `f(...)` as a call and `f(...) { }` as a definition, so
  // the brace is what separates them, and it can sit on the next line.
  if (Closed) {
    StringRef Rest = T.drop_front(Close + 1).ltrim();
    // `main(...)@c2 {` attaches a position to the closing parenthesis.
    if (Rest.starts_with("@")) {
      Rest = Rest.drop_front(1);
      size_t End = 0;
      while (End != Rest.size() && isIdentCont(Rest[End]))
        ++End;
      Rest = Rest.drop_front(End).ltrim();
    }
    if (Rest.starts_with("{") || (Rest.empty() && BodyFollows))
      return DotsOwner::Parameter;
  }
  return DotsOwner::Call;
}

/// Names what stops \p T from being the call statement whose parentheses open
/// at \p Owner and close at \p Close, or nullptr when nothing does.
///
/// An argument list inside a larger expression, as in `x = alloc(...)`, has a
/// representable shape and no statement to anchor on, so the shape alone must
/// not decide the refusal.
const char *whyNotAWholeCall(StringRef T, size_t Owner, size_t Close,
                             ArrayRef<MetaVar> MetaVars) {
  StringRef Tail = T.drop_front(Close + 1).trim();
  Tail.consume_front(";");
  if (!Tail.trim().empty())
    return "argument-level ellipsis inside an expression rather than a call "
           "statement";
  StringRef Head = T.take_front(Owner).trim();
  if (Head.contains("@"))
    // The attachment is dropped before the compiler sees the text, so a rule
    // inheriting this position would match more than it says.
    return "argument-level ellipsis on a call carrying a position attachment";
  if (Head.empty())
    return "argument-level ellipsis on a call with no callee named";
  for (char C : Head)
    if (!isIdentCont(C))
      return "argument-level ellipsis inside an expression rather than a call "
             "statement";
  for (const MetaVar &M : MetaVars)
    if (M.Name == Head)
      return "argument-level ellipsis on a call whose callee is a "
             "metavariable";
  return nullptr;
}

/// Names every "..." in \p T that this tool cannot represent, leaving the ones
/// it can unnamed so they reach the pattern compiler.
void classifyDots(StringRef T, bool BodyFollows, ArrayRef<MetaVar> MetaVars,
                  SmallVectorImpl<const char *> &Names) {
  size_t I = 0;
  while (true) {
    size_t At = T.find("...", I);
    if (At == StringRef::npos)
      return;
    I = At + 3;
    bool Closed = false;
    const char *Name = nullptr;
    switch (dotsOwner(T, At, BodyFollows, Closed)) {
    case DotsOwner::Condition:
      Name = "condition ellipsis";
      break;
    case DotsOwner::Attribute:
      Name = "attribute-argument ellipsis";
      break;
    case DotsOwner::Parameter:
      Name = "parameter-level ellipsis";
      break;
    case DotsOwner::Record:
      Name = "field-level ellipsis";
      break;
    case DotsOwner::Enum:
      Name = "enumerator-level ellipsis";
      break;
    case DotsOwner::Initialiser:
      Name = "initialiser-level ellipsis";
      break;
    case DotsOwner::Array:
      Name = "array-size ellipsis";
      break;
    case DotsOwner::Block:
      Name = "ellipsis inside a one-line block";
      break;
    case DotsOwner::Statement:
      // Every ellipsis left in a statement's text gets a name, because the
      // text is handed on verbatim and a '...' in it is not C.
      Name = "expression-level ellipsis";
      break;
    case DotsOwner::Call: {
      if (!Closed) {
        Name = "argument-level ellipsis continued on another line";
        break;
      }
      // Re-derive the argument list from the owning parenthesis.
      int Depth = 0;
      size_t Owner = StringRef::npos;
      for (size_t J = At; J-- > 0;) {
        const char C = T[J];
        if (C == ')' || C == ']' || C == '}')
          ++Depth;
        else if (C == '(' || C == '[' || C == '{') {
          if (Depth == 0) {
            Owner = J;
            break;
          }
          --Depth;
        }
      }
      Depth = 0;
      size_t Close = StringRef::npos;
      for (size_t J = Owner, E = T.size(); J != E; ++J) {
        if (T[J] == '(' || T[J] == '[' || T[J] == '{')
          ++Depth;
        else if (T[J] == ')' || T[J] == ']' || T[J] == '}')
          if (--Depth == 0) {
            Close = J;
            break;
          }
      }
      switch (argumentDotsShape(T.substr(Owner + 1, Close - Owner - 1))) {
      case ArgDotsShape::Bare:
      case ArgDotsShape::Prefix:
      case ArgDotsShape::Suffix:
      case ArgDotsShape::Surrounded:
        // In the subset, but only when the statement is the call itself.
        Name = whyNotAWholeCall(T, Owner, Close, MetaVars);
        break;
      case ArgDotsShape::Interior:
        Name = "argument-level ellipsis between two named arguments";
        break;
      case ArgDotsShape::NotDotted:
        break; // The scan found a `...`, so this cannot happen.
      }
      break;
    }
    }
    if (Name)
      Names.push_back(Name);
  }
}

void SmplParser::scanRefusedConstructs(unsigned LineNo, StringRef T, Rule &R,
                                       bool BodyFollows) {
  if (T.contains("\\(") || T.contains("\\|") || T.contains("\\)"))
    refuse(LineNo, "backslash disjunction \\( \\| \\)",
           "the backslash form may appear anywhere, including inside an "
           "expression, and a disjunction is representable only with its "
           "delimiters in column 0");
  if (T.contains("\\&"))
    refuse(LineNo, "conjunction ( & )",
           "every branch has to match at the same control-flow node, which "
           "this tool does not check");
  if (T.contains("<...") || T.contains("...>") || T.contains("<+...") ||
      T.contains("...+>"))
    refuse(LineNo, "nested dots <... ...>",
           "a nest matches its pattern zero or more times along a path, which "
           "is not a statement-level ellipsis");
  if (T.starts_with("(") && T.ends_with(")") && T.contains('|'))
    refuse(LineNo, "one-line disjunction",
           "Coccinelle matches the column-0 parenthesis to a normal one and "
           "only warns, so the rule means something other than it looks like");

  SmallVector<const char *, 4> DotNames;
  classifyDots(T, BodyFollows, R.MetaVars, DotNames);
  for (const char *Name : DotNames)
    refuse(LineNo, Name,
           "an ellipsis at this level matches a list of terms rather than a "
           "path through the control-flow graph");

  // An argument list whose ellipsis is representable can still name an
  // argument that is not, as a type metavariable or an address-of does. The
  // compiler owns that rule, so it is asked rather than copied, which is what
  // keeps the refusal and the emitted matcher from drifting apart.
  if (DotNames.empty() && T.contains("...")) {
    std::string Why;
    std::optional<ParsedPattern> P = parsePattern(R.MetaVars, {T.str()}, Why);
    if (!P)
      refuse(LineNo, "argument-level ellipsis in a pattern Clang cannot read",
             Why);
    else if (!P->Items[0])
      refuse(LineNo, "argument-level ellipsis in a pattern Clang cannot read",
             P->Errors[0]);
  }

  if (T.starts_with("#"))
    refuse(LineNo, "preprocessor directive pattern",
           "the pattern matches a directive rather than C code, which this "
           "tool sees only after preprocessing");
  if (hasWord(T, "switch") || hasWord(T, "case") || hasWord(T, "default"))
    refuse(LineNo, "switch / case pattern",
           "this statement form is not one of the forms the subset "
           "enumerates");
  if (hasWord(T, "goto"))
    refuse(LineNo, "goto pattern",
           "this statement form is not one of the forms the subset "
           "enumerates");
  if (hasWord(T, "do"))
    refuse(LineNo, "do/while pattern",
           "this statement form is not one of the forms the subset "
           "enumerates");
  for (const char *K : CxxKeywords)
    if (hasWord(T, K))
      refuse(LineNo, "C++ construct in the pattern",
             "C++ patterns are available only with Coccinelle's C++ front end "
             "enabled, and this subset is C");
  if (hasWord(T, "EXEC") || hasWord(T, "decimal"))
    refuse(LineNo, "EXEC / decimal statement pattern",
           "Coccinelle lexes these as keywords, so the pattern is not the "
           "call it looks like");
  if (T.contains("[[") || T.contains("::"))
    refuse(LineNo, "C++ construct in the pattern",
           "C++ patterns are available only with Coccinelle's C++ front end "
           "enabled, and this subset is C");
  if (looksLikeFunctionHeader(T)) {
    if (T.ends_with(";"))
      refuse(LineNo, "function prototype pattern",
             "matching a whole prototype needs a declaration pattern, not a "
             "statement pattern");
    else if (T.ends_with("{") || T.ends_with(")"))
      refuse(LineNo, "function definition pattern",
             "matching a whole function header needs a declaration pattern, "
             "not a statement pattern");
  }

  for (size_t I = 0, E = T.size(); I != E; ++I) {
    char C = T[I];
    if (C == '"' || C == '\'') {
      char Q = C;
      for (++I; I != E && T[I] != Q; ++I)
        if (T[I] == '\\' && I + 1 != E)
          ++I;
      continue;
    }
    if (!isIdentStart(C) || (I != 0 && isIdentCont(T[I - 1])))
      continue;
    StringRef Id = takeIdent(T.drop_front(I));
    I += Id.size() - 1;
    if (AllMetaVarNames.count(Id) && !CurDecls.count(Id) &&
        !CurRefused.count(Id) && !CurLiterals.count(Id) && !TypeNames.count(Id))
      refuse(LineNo, "metavariable name used as a plain C identifier",
             "the name is declared as a metavariable in another rule but not "
             "in this one, so Coccinelle reads it as literal C text and only "
             "warns");
  }
}

bool SmplParser::parsePositions(const PhysLine &L, PatternItem &It, Rule &R) {
  (void)R;
  std::string Out;
  StringRef S(It.Text);
  for (size_t I = 0, E = S.size(); I != E; ++I) {
    char C = S[I];
    if (C == '"' || C == '\'') {
      char Q = C;
      Out += C;
      for (++I; I != E; ++I) {
        Out += S[I];
        if (S[I] == '\\' && I + 1 != E) {
          Out += S[++I];
          continue;
        }
        if (S[I] == Q)
          break;
      }
      continue;
    }
    if (C != '@') {
      Out += C;
      continue;
    }
    if (It.Marker == ItemMarker::Plus)
      return err(L.Number, "a position variable is not allowed on a '+' line");
    if (I == 0 || isSpace(S[I - 1]))
      refuse(L.Number, "whitespace between token and @",
             "Coccinelle accepts it, and accepting it here would let the two "
             "spellings of one attachment diverge");
    if (I + 1 == E || !isIdentStart(S[I + 1]))
      return err(L.Number, "whitespace between '@' and the position name: "
                           "Coccinelle 1.3.3 fails an internal assertion on "
                           "this rather than diagnosing it");
    StringRef Name = takeIdent(S.drop_front(I + 1));
    auto DIt = CurDecls.find(Name);
    if (DIt == CurDecls.end() && !CurRefused.count(Name))
      return err(L.Number, "position variable '" + Name +
                               "' is not declared in this rule");
    if (DIt != CurDecls.end() && DIt->second != MetaKind::Position) {
      // Coccinelle folds the '@' into the preceding token only when the name
      // is a position metavariable. With any other kind the tokens survive
      // and mean something else, which it does not diagnose.
      refuse(L.Number, "position attachment to a non-position metavariable",
             "the name after '@' is declared as another kind, so the "
             "attachment does not bind a position");
      I += Name.size();
      continue;
    }
    if (I == 0 || !isIdentCont(S[I - 1])) {
      StringRef Pre = S.take_front(I).rtrim();
      bool Delim = Pre.ends_with("...") || Pre.ends_with(">") ||
                   Pre.ends_with("|") || Pre.ends_with("(") ||
                   Pre.ends_with(";") || Pre.ends_with("{") ||
                   Pre.ends_with("}") || Pre.ends_with(",");
      refuse(L.Number,
             Delim ? "position attached to an ellipsis, a delimiter or a "
                     "separator"
                   : "position attached to an operator",
             "this tool tracks positions on named tokens only");
    } else if (It.PositionVar.empty())
      It.PositionVar = Name.str();
    else
      refuse(L.Number, "more than one position attachment on one line",
             "PatternItem holds one position variable, so the others would be "
             "lost");
    I += Name.size();
  }
  It.Text = StringRef(Out).trim().str();
  return true;
}

bool SmplParser::parseWhen(ArrayRef<PhysLine> BodyLines, size_t &Bi,
                           StringRef WhenText, PatternItem &Dots) {
  unsigned LineNo = BodyLines[Bi].Number;
  std::string Text = WhenText.trim().str();
  bool Spliced = false;
  // A '\' continuation resets the line Coccinelle tracks, so it splices the
  // clauses rather than separating them.
  while (!Text.empty() && Text.back() == '\\' && Bi + 1 != BodyLines.size()) {
    Text.pop_back();
    ++Bi;
    Text += ' ';
    Text += BodyLines[Bi].Text.trim().str();
    Spliced = true;
  }
  StringRef W(Text);
  unsigned Count = 0;
  for (size_t Off = 0; Off < W.size();) {
    size_t At = findWord(W.drop_front(Off), "when");
    if (At == StringRef::npos)
      break;
    ++Count;
    Off += At + 4;
  }
  if (Count > 1)
    return err(LineNo, "two 'when' clauses on one line: a 'when' ends at the "
                       "end of its physical line, so the second one is "
                       "swallowed as part of the first");
  if (Spliced)
    refuse(LineNo, "backslash line continuation",
           "the continuation splices the lines, so two clauses written across "
           "it become one clause that Coccinelle then rejects");
  if (!startsWithWord(W, "when"))
    return err(LineNo, "expected 'when'");
  StringRef Rest = W.drop_front(4).ltrim();

  int Depth = 0;
  for (char C : Rest) {
    if (C == '{')
      ++Depth;
    else if (C == '}' && --Depth < 0)
      return err(LineNo, "a closing brace on the 'when' line is absorbed into "
                         "the when code; put it on a line of its own");
  }

  if (Rest.starts_with("==")) {
    // 'when ==' is not SmPL: the always-form is 'when =' with one '='.
    refuse(LineNo, "when == code",
           "the always-form is written with a single '=', and Coccinelle "
           "rejects this spelling");
    return true;
  }
  if (Rest.starts_with("!=") || startsWithWord(Rest, "not_eq")) {
    bool Alias = !Rest.starts_with("!=");
    if (Alias)
      refuse(LineNo, "C++ alternative token 'not_eq' in a when clause",
             "Coccinelle 1.3.3 aliases not_eq to '!=' and 1.1.1 does not, so "
             "the two read the same patch differently");
    StringRef Code = Rest.drop_front(Alias ? 6 : 2).trim();
    size_t Comma = Code.rfind(',');
    if (Comma != StringRef::npos) {
      StringRef Tail = Code.drop_front(Comma + 1).trim();
      if (Tail == "any" || Tail == "ANY" || Tail == "strict" ||
          Tail == "STRICT" || Tail == "forall" || Tail == "exists")
        return err(LineNo, "'when != <code>' cannot be combined with a when "
                           "modifier");
    }
    if (Code.empty())
      return err(LineNo, "'when !=' with no code after it");
    if (startsWithWord(Code, "true") || startsWithWord(Code, "false")) {
      refuse(LineNo, "when != true / when != false",
             "excluding paths on which the expression is known true or false "
             "needs value analysis this tool does not do");
      return true;
    }
    if (Code.contains("\\(") || Code.contains("\\|"))
      refuse(LineNo, "backslash disjunction \\( \\| \\)",
             "the backslash form may appear anywhere, including inside when "
             "code, and a disjunction is representable only with its "
             "delimiters in column 0");
    if (Code.contains('@'))
      refuse(LineNo, "position inside when code",
             "a position bound inside forbidden code has no match to bind to");
    if (Code.contains("..."))
      refuse(LineNo, "nested ellipsis inside when code",
             "when code is restricted here to one statement or expression, "
             "and a nested ellipsis makes it a statement sequence");
    Dots.WhenNot.push_back(Code.str());
    return true;
  }
  if (Rest.starts_with("=")) {
    refuse(LineNo, "when = <statement>",
           "requiring a statement on every path across the ellipsis is a "
           "different property from forbidding one");
    return true;
  }
  bool Any = false, Strict = false;
  while (!Rest.trim().empty()) {
    auto Split = Rest.split(',');
    StringRef Mod = Split.first.trim();
    Rest = Split.second;
    if (Mod == "any" || Mod == "ANY") {
      Any = true;
    } else if (Mod == "strict" || Mod == "STRICT") {
      Strict = true;
    } else if (Mod == "forall" || Mod == "exists") {
      refuse(LineNo, ("when " + Mod).str(),
             "a per-ellipsis path quantifier overrides the rule's own, which "
             "SemanticPatch cannot express");
    } else {
      return err(LineNo, "unknown 'when' modifier '" + Mod + "'");
    }
  }
  if (Any)
    Dots.WhenAny = true;
  if (Strict)
    Dots.WhenStrict = true;
  return true;
}

bool SmplParser::parseBody(Rule &R) {
  SmallVector<PhysLine, 32> BodyLines;
  while (Pos < Lines.size()) {
    const PhysLine L = Lines[Pos];
    StringRef T = L.Text.trim();
    // A column-zero '@' opens the next header. An indented one is a position
    // attachment, which Coccinelle binds to the last token of the line above.
    if (!T.empty() && T.front() == '@' && (L.Text.front() == '@' || T == "@@"))
      break;
    BodyLines.push_back(L);
    ++Pos;
  }

  /// One open disjunction: the item being built and the branch being filled.
  struct DisjFrame {
    PatternItem Item;
    std::vector<PatternItem> Branch;
  };
  SmallVector<DisjFrame, 4> Stack;
  auto Target = [&]() -> std::vector<PatternItem> & {
    return Stack.empty() ? R.Body : Stack.back().Branch;
  };
  /// The list the last `...` went into, and its index there, so that a `when`
  /// on a later line reaches it wherever the disjunction nesting put it.
  std::vector<PatternItem> *DotsIn = nullptr;
  int LastDots = -1;
  /// A refused construct that can still carry `when` clauses, so the clauses
  /// hanging off it are read and named rather than reported as orphans.
  PatternItem Scratch;
  bool WhenTargetRefused = false;

  bool HasMinus = false, HasPlus = false, HasContext = false;

  /// What the innermost open brace belongs to. A '...' inside anything but a
  /// block is a list ellipsis, not the statement-level one.
  enum class Brace { Block, Record, Enum, Initialiser };
  SmallVector<Brace, 4> Braces;
  auto updateBraces = [&](StringRef Text) {
    for (size_t I = 0, E = Text.size(); I != E; ++I) {
      if (Text[I] == '{') {
        StringRef Pre = Text.take_front(I).rtrim();
        if (Pre.ends_with("="))
          Braces.push_back(Brace::Initialiser);
        else if (hasWord(Pre, "enum"))
          Braces.push_back(Brace::Enum);
        else if (hasWord(Pre, "struct") || hasWord(Pre, "union"))
          Braces.push_back(Brace::Record);
        else
          Braces.push_back(Brace::Block);
      } else if (Text[I] == '}' && !Braces.empty()) {
        Braces.pop_back();
      }
    }
  };

  // Whether the next body line opens a block, which is what separates a
  // bodyless `f(...)` call pattern from a `f(...)` function header. Set at the
  // top of each iteration so addStatement sees the current line's lookahead.
  bool NextOpensBlock = false;

  auto addStatement = [&](unsigned LineNo, ItemMarker M, StringRef Text) {
    scanRefusedConstructs(LineNo, Text, R, NextOpensBlock);
    PatternItem It;
    It.Kind = ItemKind::Statement;
    It.Marker = M;
    It.Line = LineNo;
    It.Text = Text.str();
    if (!parsePositions({Text, LineNo}, It, R))
      return false;
    Target().push_back(std::move(It));
    return true;
  };

  for (size_t Bi = 0; Bi != BodyLines.size(); ++Bi) {
    const PhysLine L = BodyLines[Bi];
    StringRef Raw = L.Text;
    StringRef T = Raw.trim();
    if (T.empty())
      continue;

    NextOpensBlock = false;
    for (size_t Nj = Bi + 1; Nj != BodyLines.size(); ++Nj) {
      StringRef N = BodyLines[Nj].Text.trim();
      if (N.empty())
        continue;
      if (N.front() == '-' || N.front() == '+' || N.front() == '*')
        N = N.drop_front().ltrim();
      NextOpensBlock = N.starts_with("{");
      break;
    }

    if (Raw.starts_with("---") || Raw.starts_with("+++")) {
      refuse(L.Number, "--- / +++ filespec header",
             "restricting a rule to named old and new file paths is dead "
             "grammar in Coccinelle, which rejects it beside any code");
      continue;
    }
    if (Raw.starts_with("++")) {
      refuse(L.Number, "++ line marker",
             "adding code once per match rather than once per position is a "
             "different insertion rule from '+'");
      continue;
    }
    if (Raw.front() == '?') {
      refuse(L.Number, "? optional line marker",
             "an optional line makes the pattern match with or without it, "
             "which doubles the pattern rather than annotating it");
      continue;
    }

    ItemMarker M = ItemMarker::Context;
    StringRef Content = Raw;
    char C0 = Raw.front();
    if (C0 == '-' || C0 == '+' || C0 == '*') {
      if (!CurRuleVirtualGuarded && !setFileMode(L, C0 == '*'))
        return false;
      M = C0 == '-'   ? ItemMarker::Minus
          : C0 == '+' ? ItemMarker::Plus
                      : ItemMarker::Star;
      Content = Raw.drop_front(1);
      if (C0 != '*' && !CurRuleVirtualGuarded)
        ModificationSeen = true;
    } else {
      // A marker counts only in column 0. Coccinelle reads an indented '-' as
      // unary minus and an indented '*' as a pointer dereference, so the line
      // silently becomes context and the rule transforms nothing. The line is
      // kept as context here, which is Coccinelle's own reading, and the
      // refusal says what the author probably meant.
      StringRef LT = Raw.ltrim();
      if (LT.size() != Raw.size() && LT.size() > 1 && isSpace(LT[1]) &&
          (LT.front() == '-' || LT.front() == '*'))
        refuse(L.Number, "indented line marker",
               "a marker counts only in column 0, so Coccinelle reads this "
               "one as unary minus or as a pointer dereference and the line "
               "silently becomes context");
    }

    StringRef Body = Content.trim();
    // Column zero decides a delimiter. A marker shifts the character out of
    // column 0, so a marked line is ordinary code however it begins, and code
    // may follow a delimiter on its line.
    if (M == ItemMarker::Context &&
        (Raw.front() == '(' || Raw.front() == '|' || Raw.front() == ')' ||
         Raw.front() == '&')) {
      char D = Raw.front();
      StringRef AfterD = Raw.drop_front(1);
      if (!AfterD.empty() && AfterD.front() == '@')
        return err(L.Number, "a position variable cannot attach to a "
                             "disjunction delimiter: there is no token there "
                             "to attach it to");
      DotsIn = nullptr;
      LastDots = -1;
      WhenTargetRefused = false;
      if (D == '(') {
        DisjFrame F;
        F.Item.Kind = ItemKind::Disjunction;
        F.Item.Line = L.Number;
        Stack.push_back(std::move(F));
      } else if (D == '&') {
        refuse(L.Number, "conjunction ( & )",
               "every branch has to match at the same control-flow node, "
               "which this tool does not check");
        if (!Stack.empty()) {
          Stack.back().Item.Branches.push_back(std::move(Stack.back().Branch));
          Stack.back().Branch.clear();
        }
      } else if (D == '|') {
        if (Stack.empty())
          refuse(L.Number, "unbalanced disjunction delimiter",
                 "the '|' in column 0 has no '(' open above it, so the "
                 "branches this rule means cannot be recovered");
        else {
          Stack.back().Item.Branches.push_back(std::move(Stack.back().Branch));
          Stack.back().Branch.clear();
        }
      } else {
        if (Stack.empty())
          refuse(L.Number, "unbalanced disjunction delimiter",
                 "the ')' in column 0 has no '(' open above it, so the "
                 "branches this rule means cannot be recovered");
        else {
          DisjFrame F = std::move(Stack.back());
          Stack.pop_back();
          F.Item.Branches.push_back(std::move(F.Branch));
          Target().push_back(std::move(F.Item));
        }
      }
      Body = AfterD.trim();
      if (Body.empty())
        continue;
    } else if (Body == "|" || Body == "&") {
      refuse(L.Number, "indented disjunction delimiter",
             "in column 0 these open, separate and close a disjunction, and "
             "indented they are an ordinary parenthesis or a bitwise or, so "
             "any alternation meant here is not applied");
    }

    switch (M) {
    case ItemMarker::Minus:
      HasMinus = true;
      break;
    case ItemMarker::Plus:
      HasPlus = true;
      break;
    default:
      HasContext = true;
      break;
    }
    // A line holding nothing but its marker removes or adds a blank line.
    if (Body.empty())
      continue;
    if (Body.ends_with("\\"))
      refuse(L.Number, "backslash line continuation",
             "the continuation splices two physical lines, and the line is "
             "what decides where a 'when' clause ends");

    size_t WhenAt = findWord(Body, "when");
    StringRef Head =
        WhenAt == StringRef::npos ? Body : Body.take_front(WhenAt).rtrim();

    if (Head.empty() && WhenAt != StringRef::npos) {
      if (DotsIn && LastDots >= 0) {
        if (!parseWhen(BodyLines, Bi, Body, (*DotsIn)[LastDots]))
          return false;
      } else if (WhenTargetRefused) {
        Scratch = PatternItem();
        if (!parseWhen(BodyLines, Bi, Body, Scratch))
          return false;
      } else {
        return err(L.Number, "'when' with no '...' before it");
      }
      continue;
    }

    if (Body.front() == '@') {
      if (Body.ends_with("@"))
        return err(L.Number,
                   "a rule header carrying options must start at column 0");
      StringRef Name = takeIdent(Body.drop_front());
      if (Name.empty())
        return err(L.Number, "expected a position name after '@'");
      if (!CurDecls.count(Name) && !CurRefused.count(Name))
        return err(L.Number, "position variable '" + Name +
                                 "' is not declared in this rule");
      refuse(L.Number, "position attachment on a line of its own",
             "Coccinelle binds it to the last token of the line above, and "
             "this tool attaches a position only within one line");
      continue;
    }

    if (Head.contains("<...") || Head.contains("<+...") ||
        Head.contains("...>") || Head.contains("...+>")) {
      scanRefusedConstructs(L.Number, Head, R, NextOpensBlock);
      DotsIn = nullptr;
      LastDots = -1;
      WhenTargetRefused = true;
      if (WhenAt != StringRef::npos) {
        Scratch = PatternItem();
        if (!parseWhen(BodyLines, Bi, Body.drop_front(WhenAt), Scratch))
          return false;
      }
      continue;
    }

    if (Head.starts_with("...")) {
      StringRef Tail = Head.drop_front(3);
      if (!Tail.empty() && Tail.front() == '@') {
        refuse(L.Number,
               "position attached to an ellipsis, a delimiter or a separator",
               "this tool tracks positions on named tokens only, and an "
               "ellipsis is not one");
        Head = Head.take_front(3);
      } else if (startsWithWord(Tail.ltrim(), "exists") ||
                 startsWithWord(Tail.ltrim(), "forall")) {
        return err(L.Number, "a bare 'exists' or 'forall' after '...' is not "
                             "SmPL: Coccinelle reads it as a type name. The "
                             "per-ellipsis form is 'when exists'");
      }
    }

    // `{ ... }` written on one line is a block holding nothing but the
    // statement-level ellipsis. It is split into three items rather than
    // kept as one statement, because a '...' inside a statement's text is
    // not C and the compiler is handed that text verbatim.
    size_t Open = Head.find('{');
    if (Open != StringRef::npos && WhenAt == StringRef::npos) {
      StringRef Inner = Head.drop_front(Open + 1);
      size_t Close = Inner.find('}');
      StringRef Pre = Head.take_front(Open).rtrim();
      if (Close != StringRef::npos && Inner.take_front(Close).trim() == "..." &&
          !isRecordHeader(Pre) && !hasWord(Pre, "struct") &&
          !hasWord(Pre, "union") && !hasWord(Pre, "enum") &&
          !Pre.ends_with("=")) {
        if (M == ItemMarker::Plus)
          return err(L.Number, "'...' is not allowed on a '+' line");
        std::string Opener = (Pre + " {").str();
        if (!addStatement(L.Number, M, StringRef(Opener).ltrim()))
          return false;
        PatternItem Dots;
        Dots.Kind = ItemKind::Dots;
        Dots.Marker = M;
        Dots.Line = L.Number;
        Target().push_back(std::move(Dots));
        DotsIn = &Target();
        LastDots = static_cast<int>(DotsIn->size()) - 1;
        WhenTargetRefused = false;
        StringRef Post = Inner.drop_front(Close);
        if (!addStatement(L.Number, M, Post))
          return false;
        DotsIn = nullptr;
        LastDots = -1;
        updateBraces(Pre);
        continue;
      }
    }

    // A '...' that ends the line's code is the statement-level ellipsis, and
    // whatever precedes it on the line is a statement of its own, as in
    // `if (E) { ... when != f(e)`.
    if (Head.ends_with("...")) {
      // Only an ellipsis outside every bracket is the statement-level one. A
      // trailing '...' with a parenthesis still open is an argument or
      // parameter list continued on the next line, so it is named rather than
      // recorded as a path.
      int Bracket = 0;
      for (char C : Head.drop_back(3)) {
        if (C == '(' || C == '[')
          ++Bracket;
        else if (C == ')' || C == ']')
          --Bracket;
      }
      // An ellipsis right after an operator or a comma belongs to the
      // expression, not to the control-flow path. A ':' is excluded because
      // it ends a label, and `l: ...` is a statement-level ellipsis.
      StringRef Before = Head.drop_back(3).rtrim();
      bool AfterOperator = !Before.empty() && Before.back() != ':' &&
                           (isOpChar(Before.back()) || Before.back() == ',');
      if (Bracket > 0 || AfterOperator) {
        if (WhenAt != StringRef::npos)
          return err(L.Number, "'when' is a keyword in a rule body and "
                               "follows '...' only; Coccinelle's "
                               "argument-level when production is disabled");
        if (!addStatement(L.Number, M, Head))
          return false;
        DotsIn = nullptr;
        LastDots = -1;
        WhenTargetRefused = true;
        continue;
      }
      if (!Braces.empty() && Braces.back() != Brace::Block) {
        const char *Name =
            Braces.back() == Brace::Record ? "field-level ellipsis"
            : Braces.back() == Brace::Enum ? "enumerator-level ellipsis"
                                           : "initialiser-level ellipsis";
        refuse(L.Number, Name,
               "an ellipsis at this level matches a list of terms rather than "
               "a path through the control-flow graph");
        DotsIn = nullptr;
        LastDots = -1;
        WhenTargetRefused = true;
        updateBraces(Head);
        continue;
      }
      if (M == ItemMarker::Plus)
        return err(L.Number, "'...' is not allowed on a '+' line");
      StringRef Prefix = Head.drop_back(3).rtrim();
      if (!Prefix.empty() && !addStatement(L.Number, M, Prefix))
        return false;
      PatternItem It;
      It.Kind = ItemKind::Dots;
      It.Marker = M;
      It.Line = L.Number;
      Target().push_back(std::move(It));
      DotsIn = &Target();
      LastDots = static_cast<int>(DotsIn->size()) - 1;
      WhenTargetRefused = false;
      if (WhenAt != StringRef::npos &&
          !parseWhen(BodyLines, Bi, Body.drop_front(WhenAt),
                     (*DotsIn)[LastDots]))
        return false;
      updateBraces(Head);
      continue;
    }

    if (WhenAt != StringRef::npos)
      return err(L.Number, "'when' is a keyword in a rule body and follows "
                           "'...' only; Coccinelle's argument-level when "
                           "production is disabled");

    if (!addStatement(L.Number, M, Head))
      return false;
    updateBraces(Head);
    DotsIn = nullptr;
    LastDots = -1;
    WhenTargetRefused = false;
  }

  // An unclosed disjunction keeps its branches rather than dropping them.
  while (!Stack.empty()) {
    refuse(Stack.back().Item.Line, "unbalanced disjunction delimiter",
           "a disjunction opened in this rule is never closed in column 0, "
           "so the branches it means cannot be recovered");
    DisjFrame F = std::move(Stack.back());
    Stack.pop_back();
    F.Item.Branches.push_back(std::move(F.Branch));
    Target().push_back(std::move(F.Item));
  }
  if (HasPlus && !HasMinus && !HasContext)
    return err(R.Line, "a '+' slice with no '-' line and no context line: "
                       "Coccinelle reports \"minus slice can't be empty\"");
  return true;
}

/// The offset of a '@' that is neither inside a string nor in a '#' comment,
/// which is how Coccinelle's script lexer finds the end of a script body.
size_t findBareAt(StringRef S) {
  for (size_t I = 0, E = S.size(); I != E; ++I) {
    char C = S[I];
    if (C == '#')
      return StringRef::npos;
    if (C == '"' || C == '\'') {
      char Q = C;
      for (++I; I != E && S[I] != Q; ++I)
        if (S[I] == '\\' && I + 1 != E)
          ++I;
      continue;
    }
    if (C == '@')
      return I;
  }
  return StringRef::npos;
}

bool SmplParser::parseScriptRule(const PhysLine &HL, StringRef H) {
  StringRef AfterColon = H.split(':').second.trim();
  StringRef Lang = takeIdent(AfterColon);
  if (Lang.empty())
    return err(HL.Number, "expected a language after 'script:'");
  StringRef Opts = AfterColon.drop_front(Lang.size()).trim();

  // The name is registered whatever the language, because a later rule may
  // inherit from this one by qualified name even when the body is refused.
  std::string ScriptName;
  Rule Dummy;
  Dummy.Line = HL.Number;
  CurRuleVirtualGuarded = false;
  if (!Opts.empty()) {
    SmallVector<StringRef, 8> Toks;
    tokenizeHeader(Opts, Toks);
    size_t I = 0;
    if (I != Toks.size() && !takeIdent(Toks[I]).empty() &&
        !isHeaderKeyword(Toks[I])) {
      ScriptName = Toks[I].str();
      if (!RuleNames.insert(ScriptName).second)
        return err(HL.Number, "duplicate rule name '" + ScriptName + "'");
      ++I;
    }
    if (I != Toks.size()) {
      if (Toks[I] != "depends")
        return err(HL.Number, "unexpected '" + Toks[I] + "' in script header");
      ++I;
      if (I == Toks.size() || Toks[I] != "on")
        return err(HL.Number, "expected 'on' after 'depends'");
      ++I;
      if (!checkDependency(HL, ArrayRef<StringRef>(Toks).slice(I), Dummy))
        return false;
    }
  }

  // A Python rule that only formats and prints is inside the subset: it
  // reports, it does not change what the pattern matches. Anything else in
  // the body is named below.
  // Recorded before the language dispatch, so an unnamed rule and a refused
  // OCaml one are counted too. Without this the output says a file full of
  // reporting rules has none.
  Patch.ScriptRules.push_back({ScriptName, HL.Number});

  if (Lang != "python")
    refuse(HL.Number, ("script:" + Lang + " rule").str(),
           Lang == "ocaml" ? "an OCaml script rule is compiled and linked by "
                             "Coccinelle at run time, so what it does is not "
                             "visible to this tool"
                           : "only Python script rules are recognised");

  // Inherited-variable declarations, up to the closing '@@'. Their names are
  // recorded as refused so that a later `identifier r.x` naming one is
  // refused rather than reported as an unknown variable.
  while (Pos < Lines.size()) {
    const PhysLine L = Lines[Pos];
    StringRef T = L.Text.trim();
    // The closing '@@' may share a line with the declarations, as in
    // `@ script:python @ x << r.y; @@`.
    size_t Close = T.find("@@");
    StringRef DeclText = Close == StringRef::npos ? T : T.take_front(Close);
    if (Close == StringRef::npos) {
      ++Pos;
    } else {
      StringRef After = T.drop_front(Close + 2);
      if (After.trim().empty())
        ++Pos;
      else
        Lines[Pos].Text = After;
    }
    if (DeclText.trim().empty() && Close != StringRef::npos)
      break;
    if (!DeclText.trim().empty() && DeclText.trim().front() == '@')
      return err(L.Number, "the script header's declarations must be closed "
                           "by '@@'");
    SmallVector<StringRef, 4> Decls;
    splitTopLevel(DeclText, ';', Decls);
    for (StringRef D : Decls) {
      D = D.trim();
      if (D.empty())
        continue;
      StringRef Bound = D.split("<<").first.trim();
      for (StringRef Name : {lastIdent(Bound)}) {
        if (Name.empty())
          continue;
        AllMetaVarNames.insert(Name);
        if (!ScriptName.empty())
          RefusedMetaVars.insert(qualify(ScriptName, Name));
      }
      if (Lang != "python")
        continue;
      if (D.starts_with("(")) {
        refuse(L.Number, "script (str, ast) binding",
               "binding both the text and the AST of an inherited "
               "metavariable needs the AST the script would walk");
      } else if (D.contains("<<")) {
        StringRef Src = D.split("<<").second.trim();
        if (Src.contains('='))
          refuse(L.Number, "script binding with a default value",
                 "a default lets the script run with the variable unbound, "
                 "which is a second path through the rule");
        if (startsWithWord(Src, "virtual"))
          refuse(L.Number, "virtual.x command-line metavariable value",
                 "the value comes from a -D option on the command line");
        else if (startsWithWord(Src, "merge"))
          refuse(L.Number, "script merge variable",
                 "a merge variable collects values across the whole run");
      } else {
        refuse(L.Number, "script output variable (declared without <<)",
               "the script assigns the variable and later rules inherit it, "
               "so the pattern depends on Python that ran in between");
      }
    }
    if (Close != StringRef::npos)
      break;
  }

  // Body, up to the '@' that opens the next header.
  while (Pos < Lines.size()) {
    const PhysLine L = Lines[Pos];
    StringRef Raw = L.Text;
    if (!Raw.empty() && Raw.front() == '@')
      break;
    if (Raw.trim() == "@@")
      break;
    ++Pos;
    if (Lang != "python")
      continue;
    StringRef T = Raw.trim();
    if (T.empty() || T.front() == '#')
      continue;
    if (findBareAt(T) != StringRef::npos)
      return err(L.Number, "a bare '@' in a script body ends the body, and "
                           "Coccinelle then reads the rest as a rule header");
    if (T.starts_with("print(") || startsWithWord(T, "print") ||
        T.starts_with("coccilib.report.print_report(") ||
        T.starts_with("coccilib.org.print_todo("))
      continue;
    // Building the message text before printing it is part of the reporting
    // idiom every kernel rule uses.
    StringRef Lhs = takeIdent(T);
    if (!Lhs.empty() && (Lhs.starts_with("msg") || Lhs == "m") &&
        T.drop_front(Lhs.size()).ltrim().starts_with("="))
      continue;
    if (T.starts_with("cocci.") || T.starts_with("coccilib."))
      refuse(L.Number, "coccilib call other than print_report/print_todo",
             "these feed the script's decision back into the matching "
             "engine, so the match set depends on Python");
    else
      refuse(L.Number,
             "script:python body beyond print_report/print_todo/print",
             "only print, coccilib.report.print_report, "
             "coccilib.org.print_todo and building the message they print "
             "are understood");
  }
  return true;
}

} // namespace

std::optional<SemanticPatch>
parseSemanticPatch(StringRef Text, StringRef Filename, std::string &Error) {
  Error.clear();
  SmplParser P(Text, Filename, Error);
  return P.parse();
}

} // namespace clang::spatch

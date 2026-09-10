//===--- Edit.cpp - Turn a matched pattern into source edits -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Edit.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Transformer/SourceCode.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include <vector>

namespace clang::spatch {

namespace {

bool isIdentChar(char C) {
  return isalnum(static_cast<unsigned char>(C)) || C == '_';
}

/// Replaces every whole-word occurrence of each binding's name in \p Text with
/// the source text the binding holds.
///
/// Whole-word, because a metavariable named `E` must not be substituted inside
/// `END` or inside another metavariable's name.
///
/// Returns std::nullopt and sets \p Error when \p Text writes back a binding
/// whose range does not cover everything the target wrote. See
/// Binding::RangeIsShort: printing such a binding drops part of the target,
/// so the edit is refused and counted instead.
std::optional<std::string> substitute(llvm::StringRef Text,
                                      const Bindings &Bound,
                                      ASTContext &Context, std::string &Error) {
  std::string Out = Text.str();
  for (const auto &Entry : Bound) {
    const std::string Name = Entry.first().str();
    const std::string Value = sourceTextOf(Entry.second.Range, Context).str();
    for (size_t At = 0; (At = Out.find(Name, At)) != std::string::npos;) {
      const bool LeftOK = At == 0 || !isIdentChar(Out[At - 1]);
      const size_t End = At + Name.size();
      const bool RightOK = End == Out.size() || !isIdentChar(Out[End]);
      if (!LeftOK || !RightOK) {
        At = End;
        continue;
      }
      if (Entry.second.RangeIsShort) {
        Error = "the rule writes back the metavariable '" + Name +
                "', and what it bound has qualifiers that no source range "
                "covers, so the text written would drop them";
        return std::nullopt;
      }
      Out.replace(At, Name.size(), Value);
      At += Value.size();
    }
  }
  return Out;
}

/// Does \p Matched own the semicolon that follows it?
///
/// A declaration does. A statement of a block does too, and C has no node for
/// an expression statement, so a call written as a statement is a `CallExpr`
/// whose parent is the enclosing `CompoundStmt`. That parent is what
/// separates it from the same `CallExpr` used as an operand: extending
/// unconditionally rewrote `return -1;` to `return 1`.
bool ownsItsTerminator(DynTypedNode Matched, ASTContext &Context) {
  if (Matched.get<Decl>())
    return true;
  const auto *S = Matched.get<Stmt>();
  if (!S)
    return false;
  for (const DynTypedNode &P : Context.getParents(*S))
    if (P.get<CompoundStmt>())
      return true;
  return false;
}

/// The range \p Matched occupies, extended over its terminating semicolon
/// only when the target position owns one and \p PatternEndsInSemicolon says
/// the rule asked for it. See buildEdit for why the pattern decides too.
CharSourceRange matchedRange(DynTypedNode Matched, bool PatternEndsInSemicolon,
                             ASTContext &Context) {
  const CharSourceRange Token =
      CharSourceRange::getTokenRange(Matched.getSourceRange());
  if (!PatternEndsInSemicolon || !ownsItsTerminator(Matched, Context))
    return Token;
  return tooling::maybeExtendRange(Token, tok::semi, Context);
}

/// Is \p C whitespace that does not end a line?
bool isHorizontalSpace(char C) { return C == ' ' || C == '\t' || C == '\r'; }

bool isSpace(char C) { return isHorizontalSpace(C) || C == '\n'; }

bool allSpace(llvm::StringRef Text) {
  return llvm::all_of(Text, [](char C) { return isSpace(C); });
}

/// A half-open offset range into a file's contents.
struct Span {
  size_t Begin = 0;
  size_t End = 0;
};

/// The characters \p Range covers, as offsets into its file.
std::optional<Span> spanOf(CharSourceRange Range, const SourceManager &SM,
                           const LangOptions &Opts) {
  const CharSourceRange Char = Lexer::makeFileCharRange(Range, SM, Opts);
  if (Char.isInvalid())
    return std::nullopt;
  const auto [FID, Begin] = SM.getDecomposedLoc(Char.getBegin());
  const auto [EndFID, End] = SM.getDecomposedLoc(Char.getEnd());
  if (FID != EndFID || End < Begin)
    return std::nullopt;
  return Span{Begin, End};
}

/// Is the text from \p Begin to \p End one token?
bool isOneToken(SourceLocation Begin, size_t Length, const SourceManager &SM,
                const LangOptions &Opts) {
  Token Tok;
  if (Lexer::getRawToken(Begin, Tok, SM, Opts, /*IgnoreWhiteSpace=*/true))
    return false;
  return Tok.getLength() == Length;
}

/// Does a token opening with \p C continue the expression the region ended?
///
/// Only the characters a binary operator can open with, and none that closes
/// or separates one. `*` is in here because `a * b` reaches it; a leading `*`
/// of a dereference cannot follow an expression, so the position rules it out.
bool opensABinaryOperator(char C) {
  return llvm::StringRef("+-*/%&|^<>=!?:").contains(C);
}

/// Where the horizontal whitespace before \p At begins.
size_t spaceBefore(llvm::StringRef Buffer, size_t At) {
  while (At > 0 && isHorizontalSpace(Buffer[At - 1]))
    --At;
  return At;
}

/// Where the horizontal whitespace at \p At ends.
size_t spaceAfter(llvm::StringRef Buffer, size_t At) {
  while (At < Buffer.size() && isHorizontalSpace(Buffer[At]))
    ++At;
  return At;
}

/// Reads the buffer around a deletion, so the rules below can ask about lines
/// rather than about offsets.
class BufferLines {
public:
  explicit BufferLines(llvm::StringRef Buffer) : Buffer(Buffer) {}

  /// The offset of the first character of the line holding \p At.
  size_t startOfLine(size_t At) const {
    const size_t NL = Buffer.rfind('\n', At);
    return NL == llvm::StringRef::npos ? 0 : NL + 1;
  }

  /// The offset just past the newline that ends the line holding \p At, or the
  /// end of the buffer when the last line has no newline.
  size_t pastEndOfLine(size_t At) const {
    const size_t NL = Buffer.find('\n', At);
    return NL == llvm::StringRef::npos ? Buffer.size() : NL + 1;
  }

  bool onlySpaceBetween(size_t Begin, size_t End) const {
    return allSpace(Buffer.slice(Begin, End));
  }

  /// How many whitespace-only lines sit directly above \p LineBegin, and where
  /// the first of them starts.
  std::pair<unsigned, size_t> blankLinesAbove(size_t LineBegin) const {
    unsigned Count = 0;
    size_t At = LineBegin;
    while (At > 0) {
      const size_t Above = startOfLine(At - 1);
      if (!onlySpaceBetween(Above, At - 1))
        break;
      At = Above;
      ++Count;
    }
    return {Count, At};
  }

  /// How many whitespace-only lines sit directly below \p PastLineEnd, and
  /// where the last of them ends. A whitespace-only tail with no newline of
  /// its own is not counted, because removing it would join two lines.
  std::pair<unsigned, size_t> blankLinesBelow(size_t PastLineEnd) const {
    unsigned Count = 0;
    size_t At = PastLineEnd;
    while (At < Buffer.size()) {
      const size_t NL = Buffer.find('\n', At);
      if (NL == llvm::StringRef::npos || !onlySpaceBetween(At, NL))
        break;
      At = NL + 1;
      ++Count;
    }
    return {Count, At};
  }

  /// The last character before \p At that is not whitespace, or 0.
  char lastNonSpaceBefore(size_t At) const {
    for (size_t I = At; I-- > 0;)
      if (!isSpace(Buffer[I]))
        return Buffer[I];
    return 0;
  }

  /// The first character at or after \p At that is not whitespace, or 0.
  char firstNonSpaceAfter(size_t At) const {
    for (size_t I = At; I < Buffer.size(); ++I)
      if (!isSpace(Buffer[I]))
        return Buffer[I];
    return 0;
  }

  llvm::StringRef text() const { return Buffer; }

private:
  llvm::StringRef Buffer;
};

/// The span a deletion of \p Group should cover once the whitespace it would
/// leave behind is taken with it.
Span widenedSpan(const BufferLines &Lines, Span Group) {
  const size_t LineBegin = Lines.startOfLine(Group.Begin);
  const size_t PastLineEnd = Lines.pastEndOfLine(Group.End);
  const bool WholeLines = Lines.onlySpaceBetween(LineBegin, Group.Begin) &&
                          Lines.onlySpaceBetween(Group.End, PastLineEnd);

  if (!WholeLines) {
    // Kept code shares the line, so the line stays and only the space the
    // deletion opened up goes.
    Span S = Group;
    while (S.End < Lines.text().size() &&
           isHorizontalSpace(Lines.text()[S.End]))
      ++S.End;
    if (Lines.onlySpaceBetween(S.End, Lines.pastEndOfLine(S.End)))
      while (S.Begin > 0 && isHorizontalSpace(Lines.text()[S.Begin - 1]))
        --S.Begin;
    return S;
  }

  const auto [Above, AboveBegin] = Lines.blankLinesAbove(LineBegin);
  const auto [Below, BelowEnd] = Lines.blankLinesBelow(PastLineEnd);
  if (Lines.lastNonSpaceBefore(LineBegin) == '{' && Above == 0)
    return {LineBegin, BelowEnd};
  if (Below > 0 || Lines.firstNonSpaceAfter(PastLineEnd) == '}')
    return {AboveBegin, PastLineEnd};
  return {LineBegin, PastLineEnd};
}

} // namespace

llvm::StringRef sourceTextOf(const Stmt &S, ASTContext &Context) {
  return sourceTextOf(S.getSourceRange(), Context);
}

llvm::StringRef sourceTextOf(SourceRange Range, ASTContext &Context) {
  return Lexer::getSourceText(CharSourceRange::getTokenRange(Range),
                              Context.getSourceManager(),
                              Context.getLangOpts());
}

std::optional<PatternEdit> buildEdit(DynTypedNode Matched,
                                     llvm::StringRef PlusText,
                                     bool PatternEndsInSemicolon,
                                     const Bindings &Bound, ASTContext &Context,
                                     std::string &Error) {
  const CharSourceRange Range =
      matchedRange(Matched, PatternEndsInSemicolon, Context);
  // Every range is validated before a Replacement is built from it, because
  // `Replacement::setFromSourceRange` takes the spelling location
  // unconditionally. A node spelled inside a macro body would otherwise get an
  // edit pointing at the definition, rewriting every other expansion.
  if (llvm::Error Invalid =
          tooling::validateEditRange(Range, Context.getSourceManager())) {
    Error = "the matched range cannot be edited: " +
            llvm::toString(std::move(Invalid));
    return std::nullopt;
  }

  const std::optional<std::string> Text =
      substitute(PlusText, Bound, Context, Error);
  if (!Text)
    return std::nullopt;
  return PatternEdit{tooling::Replacement(Context.getSourceManager(), Range,
                                          *Text, Context.getLangOpts())};
}

CharSourceRange inPlaceEditRange(CharSourceRange Range, std::string &Text,
                                 ASTContext &Context,
                                 bool RangeIsAWrittenType) {
  const SourceManager &SM = Context.getSourceManager();
  const LangOptions &Opts = Context.getLangOpts();
  const std::optional<Span> At = spanOf(Range, SM, Opts);
  if (!At)
    return Range;
  const llvm::StringRef Buffer =
      SM.getBufferData(SM.getFileID(Lexer::makeFileCharRange(Range, SM, Opts)
                                        .getBegin()));
  // The offsets and the buffer come from one FileID, which is what lets the
  // scans below index the buffer at all.
  assert(At->End <= Buffer.size() && "offsets outside their own buffer");

  Span Wide = *At;
  // Coccinelle prints nothing between a `(` and what follows it.
  const size_t LeadBegin = spaceBefore(Buffer, At->Begin);
  if (LeadBegin > 0 && Buffer[LeadBegin - 1] == '(')
    Wide.Begin = LeadBegin;

  const size_t TrailEnd = spaceAfter(Buffer, At->End);
  const bool HasTrailing = TrailEnd > At->End;
  const char Next = TrailEnd < Buffer.size() ? Buffer[TrailEnd] : '\0';
  if (llvm::StringRef(Text).rtrim().ends_with("*")) {
    // The `+` text ends in a pointer star, which binds to whatever follows
    // it, so `- LPINT` over `+ int *` writes `int *y` from `LPINT y`.
    Wide.End = TrailEnd;
  } else if (opensABinaryOperator(Next) && !RangeIsAWrittenType) {
    if (!HasTrailing)
      Text += " ";
  } else if (Next == ',' || Next == ')' || Next == ';') {
    // A single token is swapped where it stands. Anything longer collapses to
    // the `+` text and takes the whitespace after it with it. Only these three
    // followers were measured, so any other one keeps its whitespace.
    if (!isOneToken(Range.getBegin(), At->End - At->Begin, SM, Opts))
      Wide.End = TrailEnd;
  }

  const SourceLocation FileBegin = SM.getLocForStartOfFile(
      SM.getFileID(Lexer::makeFileCharRange(Range, SM, Opts).getBegin()));
  return CharSourceRange::getCharRange(
      FileBegin.getLocWithOffset(static_cast<int>(Wide.Begin)),
      FileBegin.getLocWithOffset(static_cast<int>(Wide.End)));
}

InnerEdit innerEditRange(unsigned PatternBegin, unsigned PatternEnd,
                         const NodePairs &Pairs, const TypeLocPairs &TypePairs,
                         const DeclarationPairs &DeclarationPairs,
                         ASTContext &PatternContext, ASTContext &Context) {
  const auto CoversChars = [&](CharSourceRange Pattern) {
    const std::optional<Span> Here =
        spanOf(Pattern, PatternContext.getSourceManager(),
               PatternContext.getLangOpts());
    return Here && Here->Begin == PatternBegin && Here->End == PatternEnd;
  };
  const auto Covers = [&](SourceRange Pattern) {
    return CoversChars(CharSourceRange::getTokenRange(Pattern));
  };
  // The first exact match wins. A pattern node and an implicit cast over it
  // name the same characters, so either one gives the same range.
  for (const auto &Pair : Pairs)
    if (Covers(Pair.Pattern->getSourceRange()))
      return {Pair.Target->getSourceRange(), /*IsAWrittenType=*/false};
  for (const auto &Pair : TypePairs)
    if (Covers(Pair.Pattern.getSourceRange()))
      return {Pair.Target.getSourceRange(), /*IsAWrittenType=*/true};
  // A declaration last, so a rule marking a member's type alone takes the
  // type's range from the pair above rather than the whole member's from here.
  //
  // Each side is also tried extended over the semicolon that follows it,
  // because a member's range stops at its declarator while the patch marks the
  // line including the `;`. Without that, `- int a;` inside `T { ... };`
  // matched no pattern range at all and the whole record was replaced. Only a
  // declaration whose range stops short of its `;` arrives here, so extending
  // never takes a second one: a block-scope declaration's range does end
  // there, and its `DeclStmt` covers the same characters and answers from
  // `NodePairs` above.
  const auto throughTerminator = [](const Decl *D, ASTContext &Of) {
    return tooling::maybeExtendRange(
        CharSourceRange::getTokenRange(D->getSourceRange()), tok::semi, Of);
  };
  for (const auto &Pair : DeclarationPairs) {
    if (Covers(Pair.Pattern->getSourceRange()))
      return {Pair.Target->getSourceRange(), /*IsAWrittenType=*/false};
    if (!CoversChars(throughTerminator(Pair.Pattern, PatternContext)))
      continue;
    // Each side's range comes from its own manager, because the two are
    // separate translation units and one side's offsets mean nothing in the
    // other's.
    return {throughTerminator(Pair.Target, Context).getAsRange(),
            /*IsAWrittenType=*/false};
  }
  return {};
}

std::optional<PatternEdit>
buildInnerEdit(SourceRange Target, llvm::StringRef PlusText,
               bool RangeIsAWrittenType, const Bindings &Bound,
               ASTContext &Context, std::string &Error) {
  std::optional<std::string> Substituted =
      substitute(PlusText, Bound, Context, Error);
  if (!Substituted)
    return std::nullopt;
  std::string Text = std::move(*Substituted);
  const CharSourceRange Range =
      inPlaceEditRange(CharSourceRange::getTokenRange(Target), Text, Context,
                       RangeIsAWrittenType);
  if (llvm::Error Invalid =
          tooling::validateEditRange(Range, Context.getSourceManager())) {
    Error = "the range inside the match cannot be edited: " +
            llvm::toString(std::move(Invalid));
    return std::nullopt;
  }
  return PatternEdit{tooling::Replacement(Context.getSourceManager(), Range,
                                          Text, Context.getLangOpts())};
}

tooling::Replacements widenDeletions(llvm::StringRef FilePath,
                                     llvm::StringRef Buffer,
                                     const tooling::Replacements &Reps) {
  const BufferLines Lines(Buffer);
  const std::vector<tooling::Replacement> Sorted(Reps.begin(), Reps.end());
  tooling::Replacements Out;
  bool Failed = false;
  auto keep = [&](const tooling::Replacement &R) {
    if (llvm::Error E = Out.add(R)) {
      llvm::consumeError(std::move(E));
      Failed = true;
    }
  };

  for (size_t I = 0; I < Sorted.size();) {
    if (!Sorted[I].getReplacementText().empty() || Sorted[I].getLength() == 0) {
      keep(Sorted[I]);
      ++I;
      continue;
    }
    // Coccinelle deletes a run of statements as one region, so two deletions
    // with nothing but whitespace between them are widened together. Widening
    // them apart leaves the blank line that separated them.
    Span Group{Sorted[I].getOffset(),
               Sorted[I].getOffset() + Sorted[I].getLength()};
    size_t J = I + 1;
    for (; J < Sorted.size() && Sorted[J].getReplacementText().empty() &&
           Sorted[J].getLength() > 0 &&
           Lines.onlySpaceBetween(Group.End, Sorted[J].getOffset());
         ++J)
      Group.End = Sorted[J].getOffset() + Sorted[J].getLength();
    const Span Wide = widenedSpan(Lines, Group);
    keep(tooling::Replacement(FilePath, Wide.Begin, Wide.End - Wide.Begin, ""));
    I = J;
  }

  // A conflict means the widened spans overlap, which the grouping above is
  // meant to prevent. The unwidened set is still correct, so it is what a
  // caller gets rather than a partly widened one.
  return Failed ? Reps : Out;
}

} // namespace clang::spatch

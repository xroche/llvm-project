//===--- PatternParser.cpp - Parse an SmPL pattern with Clang -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PatternParser.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"

namespace clang::spatch {

namespace {

/// The prefix of the function each pattern statement is wrapped in. The index
/// after it maps a body back to the statement it came from.
constexpr llvm::StringLiteral ItemPrefix = "__spatch_item_";

bool isIdentChar(char C) {
  return isalnum(static_cast<unsigned char>(C)) || C == '_';
}

/// How a pattern uses one metavariable, which is what decides the type it has
/// to be given. A bare use accepts any type; the rest do not.
struct Usage {
  llvm::StringSet<> Members; ///< Names after `->` or `.`.
  bool Subscripted = false;  ///< Appears as `x[...]`.
  bool Called = false;       ///< Appears as `x(...)`.
  bool Arrow = false;        ///< At least one `->`, so the type is a pointer.
};

/// Scans \p Statements for every use of \p Name.
Usage usageOf(llvm::StringRef Name, llvm::ArrayRef<std::string> Statements) {
  Usage U;
  for (const std::string &S : Statements) {
    llvm::StringRef T(S);
    size_t I = 0;
    while (true) {
      const size_t At = T.find(Name, I);
      if (At == llvm::StringRef::npos)
        break;
      I = At + Name.size();
      // A name inside a longer identifier is a different name.
      if (At != 0 && isIdentChar(T[At - 1]))
        continue;
      if (I != T.size() && isIdentChar(T[I]))
        continue;
      llvm::StringRef Rest = T.drop_front(I).ltrim();
      if (Rest.consume_front("->") || Rest.consume_front(".")) {
        U.Arrow |= T.drop_front(I).ltrim().starts_with("->");
        Rest = Rest.ltrim();
        size_t N = 0;
        while (N != Rest.size() && isIdentChar(Rest[N]))
          ++N;
        if (N != 0)
          U.Members.insert(Rest.take_front(N));
      } else if (Rest.starts_with("[")) {
        U.Subscripted = true;
      } else if (Rest.starts_with("(")) {
        U.Called = true;
      }
    }
  }
  return U;
}

/// The C type text a metavariable with \p U has to be given, or empty when a
/// plain `int` will do.
std::string typeFor(llvm::StringRef Name, const Usage &U, std::string &Struct) {
  if (!U.Members.empty()) {
    // The pattern names the members, so a type that has them can be built.
    // Every member is an int, because the pattern never constrains the member
    // type and giving it one would reject a target that disagrees.
    std::string Body;
    llvm::raw_string_ostream OS(Body);
    OS << "struct __spatch_" << Name << "_t {";
    llvm::SmallVector<llvm::StringRef, 4> Names;
    for (const auto &M : U.Members)
      Names.push_back(M.first());
    llvm::sort(Names);
    for (llvm::StringRef M : Names)
      OS << " int " << M << ";";
    OS << " };\n";
    Struct = Body;
    return ("struct __spatch_" + Name + "_t " + (U.Arrow ? "*" : "")).str();
  }
  if (U.Called)
    return "int (*" + Name.str() + ")()"; // handled by the caller's spelling
  if (U.Subscripted)
    return "int *";
  return "int ";
}

/// The C keywords a pattern can contain, which must never be declared as if
/// they were names the pattern references.
bool isCKeyword(llvm::StringRef S) {
  static const char *const Words[] = {
      "auto",     "break",    "case",          "char",   "const",   "continue",
      "default",  "do",       "double",        "else",   "enum",    "extern",
      "float",    "for",      "goto",          "if",     "inline",  "int",
      "long",     "register", "restrict",      "return", "short",   "signed",
      "sizeof",   "static",   "struct",        "switch", "typedef", "union",
      "unsigned", "void",     "volatile",      "while",  "_Bool",   "_Alignof",
      "alignof",  "typeof",   "__attribute__", "NULL"};
  return llvm::is_contained(Words, S);
}

/// Every identifier a pattern mentions that is not a metavariable and not a
/// keyword.
///
/// Coccinelle matches these by name and needs no declaration for them. Clang
/// does, because an unresolved name is an error and the whole rule then fails
/// to parse. `demos/itimer.cocci` is the case: it declares no metavariables at
/// all and names four kernel functions, so without this nothing in it parses.
std::vector<std::string> freeIdentifiers(llvm::ArrayRef<MetaVar> MetaVars,
                                         llvm::ArrayRef<std::string> Stmts) {
  llvm::StringSet<> Seen;
  std::vector<std::string> Out;
  for (const MetaVar &M : MetaVars)
    Seen.insert(M.Name);
  Seen.insert(DotsMarker);
  for (const std::string &S : Stmts) {
    llvm::StringRef T(S);
    size_t I = 0;
    while (I != T.size()) {
      if (!isIdentChar(T[I]) || isdigit(static_cast<unsigned char>(T[I]))) {
        // Skip a whole number, so the digits of `1u` are not read as a name.
        if (isdigit(static_cast<unsigned char>(T[I])))
          while (I != T.size() && isIdentChar(T[I]))
            ++I;
        else
          ++I;
        continue;
      }
      const size_t Start = I;
      while (I != T.size() && isIdentChar(T[I]))
        ++I;
      llvm::StringRef Name = T.substr(Start, I - Start);
      // A member name after `->` or `.` belongs to a synthesised struct and is
      // not a free name of its own.
      llvm::StringRef Before = T.take_front(Start).rtrim();
      if (Before.ends_with("->") || Before.ends_with("."))
        continue;
      if (isCKeyword(Name) || !Seen.insert(Name).second)
        continue;
      Out.push_back(Name.str());
    }
  }
  return Out;
}

/// Splits \p Args on the commas that sit outside every bracket.
///
/// Returns false when the brackets do not balance, in which case \p Out holds
/// whatever was split before the imbalance.
bool splitArgumentList(llvm::StringRef Args,
                       llvm::SmallVectorImpl<llvm::StringRef> &Out) {
  int Depth = 0;
  size_t Start = 0;
  for (size_t I = 0, E = Args.size(); I != E; ++I) {
    const char C = Args[I];
    if (C == '(' || C == '[' || C == '{')
      ++Depth;
    else if (C == ')' || C == ']' || C == '}')
      --Depth;
    else if (C == ',' && Depth == 0) {
      Out.push_back(Args.substr(Start, I - Start).trim());
      Start = I + 1;
    }
    if (Depth < 0)
      return false;
  }
  if (Depth != 0)
    return false;
  llvm::StringRef Last = Args.substr(Start).trim();
  if (!Last.empty() || !Out.empty())
    Out.push_back(Last);
  return true;
}

} // namespace

ArgDotsShape argumentDotsShape(llvm::StringRef Args) {
  llvm::SmallVector<llvm::StringRef, 8> Parts;
  if (!splitArgumentList(Args, Parts))
    return ArgDotsShape::NotDotted;
  llvm::SmallVector<unsigned, 4> Dots;
  for (unsigned I = 0, E = Parts.size(); I != E; ++I)
    if (Parts[I] == "...")
      Dots.push_back(I);
  if (Dots.empty())
    return ArgDotsShape::NotDotted;
  if (Parts.size() == 1)
    return ArgDotsShape::Bare;
  const bool Leading = Dots.front() == 0;
  const bool Trailing = Dots.back() == Parts.size() - 1;
  if (!Leading && Trailing)
    return ArgDotsShape::Prefix;
  if (Leading && !Trailing)
    return ArgDotsShape::Suffix;
  if (Leading && Trailing)
    return ArgDotsShape::Surrounded;
  // Dots with a named term on each side need a position counted from each end,
  // which the unifier does not do.
  return ArgDotsShape::Interior;
}

std::string synthesiseDeclarations(llvm::ArrayRef<MetaVar> MetaVars,
                                   llvm::ArrayRef<std::string> Statements) {
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << "/* synthesised by clang-spatch to parse one rule's patterns */\n";
  OS << "int " << DotsMarker << "();\n";
  for (const MetaVar &M : MetaVars) {
    if (M.Kind == MetaVar::Kind::Position)
      continue; // A position binds a location, so it needs no declaration.
    if (M.Kind == MetaVar::Kind::Type) {
      OS << "typedef int " << M.Name << ";\n";
      continue;
    }
    const Usage U = usageOf(M.Name, Statements);
    std::string Struct;
    const std::string Type = typeFor(M.Name, U, Struct);
    if (!Struct.empty())
      OS << Struct;
    if (U.Called && Struct.empty())
      OS << "int " << M.Name << "();\n";
    else
      OS << "extern " << Type << M.Name << ";\n";
  }
  // Declare the names the pattern references literally, with the same
  // usage-driven typing, so that Clang can resolve them.
  for (const std::string &Name : freeIdentifiers(MetaVars, Statements)) {
    const Usage U = usageOf(Name, Statements);
    std::string Struct;
    const std::string Type = typeFor(Name, U, Struct);
    if (!Struct.empty())
      OS << Struct;
    if (U.Called && Struct.empty())
      OS << "int " << Name << "();\n";
    else
      OS << "extern " << Type << Name << ";\n";
  }
  return Out;
}

std::optional<ParsedPattern>
parsePattern(llvm::ArrayRef<MetaVar> MetaVars,
             llvm::ArrayRef<std::string> Statements, std::string &Error) {
  ParsedPattern P;
  P.Items.assign(Statements.size(), nullptr);
  P.Errors.assign(Statements.size(), std::string());

  std::string Src = synthesiseDeclarations(MetaVars, Statements);
  // One function per statement, so a body can be mapped back to the statement
  // it came from by the index in its name.
  llvm::SmallVector<unsigned, 8> Wrapped;
  for (unsigned I = 0; I != Statements.size(); ++I) {
    llvm::StringRef T = llvm::StringRef(Statements[I]).trim();
    if (T.empty() || T == "{" || T == "}") {
      P.Errors[I] = "the pattern line carries no statement";
      continue;
    }
    std::string Body = T.str();
    // `...` is not C. It reaches the AST as a marker call instead, which the
    // unifier reads as "zero or more arguments".
    for (size_t At = Body.find("..."); At != std::string::npos;
         At = Body.find("...", At))
      Body.replace(At, 3, (DotsMarker + "()").str());
    // The wrapper returns int so that `return E;` is a valid pattern. A void
    // wrapper made it a -Wreturn-mismatch warning, which `-w` then hid.
    Src += "int " + ItemPrefix.str() + std::to_string(I) + "(void) {\n";
    Src += Body;
    if (!llvm::StringRef(Body).rtrim().ends_with(";") &&
        !llvm::StringRef(Body).rtrim().ends_with("}"))
      Src += ";";
    Src += "\n}\n";
    Wrapped.push_back(I);
  }
  P.Source = Src;

  // Errors are wanted per statement rather than fatally, so diagnostics are
  // collected and attributed below instead of stopping the parse.
  std::unique_ptr<ASTUnit> Unit = tooling::buildASTFromCodeWithArgs(
      Src, {"-std=gnu11", "-w", "-ferror-limit=0"}, "spatch-pattern.c");
  if (!Unit) {
    Error = "Clang could not be run on the synthesised pattern";
    return std::nullopt;
  }
  // The result owns the translation unit. Every node in ParsedPattern::Items
  // points into it, so letting it die here leaves them all dangling.
  P.Unit = std::move(Unit);

  // Clang error-recovers, so a node can come back from text it could not read.
  // The wrapper each item sits in is known by line, so an error inside one
  // marks that item unparsed rather than letting a partial tree through.
  const SourceManager &DiagSM = P.Unit->getSourceManager();
  llvm::DenseMap<unsigned, std::string> ErrorAtLine;
  for (auto It = P.Unit->stored_diag_begin(), E = P.Unit->stored_diag_end();
       It != E; ++It) {
    if (It->getLevel() < DiagnosticsEngine::Error)
      continue;
    const FullSourceLoc Loc = It->getLocation();
    if (!Loc.isValid())
      continue;
    const unsigned Line = DiagSM.getSpellingLineNumber(Loc);
    ErrorAtLine.try_emplace(Line, It->getMessage().str());
  }

  ASTContext &Ctx = P.Unit->getASTContext();
  for (Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody()) {
      // A metavariable's own declaration, which is how a reference to it is
      // recognised later.
      if (const auto *ND = dyn_cast<NamedDecl>(D))
        for (const MetaVar &M : MetaVars)
          if (ND->getName() == M.Name)
            P.MetaVarDecls[ND->getCanonicalDecl()] = &M;
      continue;
    }
    llvm::StringRef Name = FD->getName();
    if (!Name.consume_front(ItemPrefix))
      continue;
    unsigned Index = 0;
    if (Name.getAsInteger(10, Index) || Index >= Statements.size())
      continue;
    // The wrapper spans from its own line to its closing brace, so any error
    // inside that span belongs to this item.
    const unsigned First = DiagSM.getSpellingLineNumber(FD->getBeginLoc());
    const unsigned Last = DiagSM.getSpellingLineNumber(FD->getEndLoc());
    bool Errored = false;
    for (unsigned L = First; L <= Last && !Errored; ++L)
      if (auto Found = ErrorAtLine.find(L); Found != ErrorAtLine.end()) {
        P.Errors[Index] =
            "the pattern statement did not parse as C: " + Found->second;
        Errored = true;
      }
    if (Errored)
      continue;

    const auto *Body = dyn_cast<CompoundStmt>(FD->getBody());
    if (!Body || Body->body_empty()) {
      P.Errors[Index] = "the pattern statement did not parse as C";
      continue;
    }
    // A pattern line is one statement. More than one means the line held a
    // sequence, which the rule body is supposed to express instead.
    if (Body->size() != 1) {
      P.Errors[Index] = "the pattern line holds more than one statement";
      continue;
    }
    const Stmt *Only = Body->body_front();
    // `double complex` is a valid type in gnu11, so it parses as a
    // declaration that declares nothing. Clang only warns, and the pattern is
    // a type rather than a statement, so the node is not usable.
    if (const auto *DS = dyn_cast<DeclStmt>(Only))
      if (DS->decl_begin() == DS->decl_end()) {
        P.Errors[Index] = "the pattern is a type rather than a statement";
        continue;
      }
    // A pattern that is only a semicolon carries nothing to match.
    if (isa<NullStmt>(Only)) {
      P.Errors[Index] = "the pattern line carries no statement";
      continue;
    }
    P.Items[Index] = Only;
  }

  for (unsigned I : Wrapped)
    if (!P.Items[I] && P.Errors[I].empty())
      P.Errors[I] = "the pattern statement did not parse as C";

  return P;
}

} // namespace clang::spatch

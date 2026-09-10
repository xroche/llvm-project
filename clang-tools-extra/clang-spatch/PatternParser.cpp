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
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"

namespace clang::spatch {

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

namespace {

/// The prefix of the function each pattern statement is wrapped in. The index
/// after it maps a body back to the statement it came from.
constexpr llvm::StringLiteral ItemPrefix = "__spatch_item_";

/// The declarator a type pattern is given so that Clang produces a location
/// for the type. It is local to one wrapper, so every item may reuse it.
///
/// A pointer, because an object of incomplete type cannot be declared and
/// `struct scsi_cmnd` is what `tests/compare.cocci` writes. The type keeps
/// the characters it occupied, so the offsets a caller holds still name it.
constexpr llvm::StringLiteral TypedDeclarator = "*__spatch_typed";

/// The message an item gets when Clang read it as a type rather than as a
/// statement. Matched rather than re-derived, because the second synthesis
/// pass keys on it.
constexpr llvm::StringLiteral ATypeNotAStatement =
    "the pattern declares nothing, so it is a type rather than a statement";

/// The declarator an initialiser group is given so that Clang reads its lines
/// as initialiser elements.
///
/// An array, because a field designator cannot resolve against it and so the
/// element keeps the name the patch wrote rather than one Clang looked up. The
/// designator survives that: the parser records the written name and Sema
/// overwrites it only once it has found the field. The group's own braces
/// follow this text unchanged, so the offsets a caller holds still name the
/// characters the pattern occupies.
constexpr llvm::StringLiteral GroupDeclarator = "int __spatch_group[] =";

/// The message an item gets when its element list holds more than one element.
constexpr llvm::StringLiteral AGroupOfSeveralElements =
    "the pattern's brace group holds more than one element, so matching it "
    "needs the elements found adjacent in the target's own list, which this "
    "version does not build";

/// \p E without the \c RecoveryExpr the group wrapper's own unresolvable
/// designator provokes.
const Expr *peelRecovery(const Expr *E) {
  if (const auto *R = dyn_cast_or_null<RecoveryExpr>(E))
    return R->subExpressions().size() == 1 ? R->subExpressions().front()
                                           : nullptr;
  return E;
}

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

/// Is \p ID a diagnostic the synthesised declarations provoke rather than the
/// pattern?
///
/// A metavariable is declared `extern int` so that it accepts any use the
/// pattern makes of it, and that permissiveness has consequences Clang
/// reports. `static const char *str = E;` is a pattern Coccinelle accepts and
/// a valid tree comes back for it, but a non-constant `E` initialising a
/// static local is an error with no warning group to switch off. The tree
/// still says what the pattern says, so the item is not marked unparsed.
///
/// The diagnostics that do have a group are switched off on the command line
/// instead, which is the same policy said the other way round.
bool isSynthesisArtefact(unsigned ID) {
  // A field designator cannot resolve against the array an initialiser group
  // is wrapped in, which is the point of choosing an array: the element then
  // keeps the name the patch wrote. The `InitListExpr` and its designators
  // come back intact under a `RecoveryExpr`, so the tree still says what the
  // pattern says.
  return ID == diag::err_init_element_not_constant ||
         ID == diag::err_field_designator_non_aggr;
}

/// The macro Clang predefines for one of C's wide-character type names, or an
/// empty string for any other name.
///
/// A string literal initialiser compares element types exactly, so
/// `char32_t e[] = U"";` does not parse against `typedef int char32_t;`.
llvm::StringRef wideCharTypeMacro(llvm::StringRef Name) {
  if (Name == "char16_t")
    return "__CHAR16_TYPE__";
  if (Name == "char32_t")
    return "__CHAR32_TYPE__";
  if (Name == "wchar_t")
    return "__WCHAR_TYPE__";
  return llvm::StringRef();
}

/// Every identifier a pattern mentions that is not a metavariable and not a
/// keyword.
///
/// Coccinelle matches these by name and needs no declaration for them. Clang
/// does, because an unresolved name is an error and the whole rule then fails
/// to parse. `demos/itimer.cocci` is the case: it declares no metavariables at
/// all and names four kernel functions, so without this nothing in it parses.
std::vector<std::string> freeIdentifiers(llvm::ArrayRef<MetaVar> MetaVars,
                                         llvm::ArrayRef<std::string> TypeNames,
                                         llvm::ArrayRef<std::string> Stmts) {
  llvm::StringSet<> Seen;
  std::vector<std::string> Out;
  for (const MetaVar &M : MetaVars)
    Seen.insert(M.Name);
  // A type name already has a typedef of its own, and declaring it a second
  // time as a variable is a redefinition Clang refuses.
  for (const std::string &Name : TypeNames)
    Seen.insert(Name);
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
                                   llvm::ArrayRef<std::string> Statements,
                                   llvm::ArrayRef<std::string> TypeNames) {
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << "/* synthesised by clang-spatch to parse one rule's patterns */\n";
  OS << "int " << DotsMarker << "();\n";
  // The list is the whole patch's rather than this rule's, which costs an
  // unused typedef in a rule that names none of them and changes no parse.
  for (const std::string &Name : TypeNames) {
    // A name declared both ways is declared once, by the metavariable loop
    // below, because there it is a wildcard and here it stands for itself.
    if (llvm::any_of(MetaVars,
                     [&](const MetaVar &M) { return M.Name == Name; }))
      continue;
    const llvm::StringRef Macro = wideCharTypeMacro(Name);
    OS << "typedef " << (Macro.empty() ? "int" : Macro) << " " << Name << ";\n";
  }
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
  for (const std::string &Name :
       freeIdentifiers(MetaVars, TypeNames, Statements)) {
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

namespace {

/// Synthesises the source for one pass over \p Statements and parses it into
/// \p P, whose \c Unit is left null when Clang could not be run at all.
///
/// \p AsType says, per statement, that Clang read it as a type rather than as
/// a statement, so it is given a declarator this time round. A type written
/// alone declares nothing and Clang produces no location for it.
void parseOnce(llvm::ArrayRef<MetaVar> MetaVars,
               llvm::ArrayRef<std::string> Statements,
               llvm::ArrayRef<std::string> TypeNames,
               llvm::ArrayRef<bool> AsType, llvm::ArrayRef<bool> AsGroup,
               ParsedPattern &P) {
  P.Items.assign(Statements.size(), nullptr);
  P.TypeItems.assign(Statements.size(), TypeLoc());
  P.Errors.assign(Statements.size(), std::string());
  P.ItemOffsets.assign(Statements.size(), 0);

  std::string Src = synthesiseDeclarations(MetaVars, Statements, TypeNames);
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
    // The declarator comes before the group's own braces, so the offsets the
    // caller holds have to be taken after it.
    if (AsGroup[I])
      Src += GroupDeclarator.str() + " ";
    P.ItemOffsets[I] = Src.size();
    Src += Body;
    if (AsType[I])
      // The declarator comes after the item's own text, so the offsets the
      // caller holds still name the characters the type occupies.
      Src += " " + TypedDeclarator.str() + ";";
    else if (AsGroup[I])
      Src += ";";
    else if (!llvm::StringRef(Body).rtrim().ends_with(";") &&
             !llvm::StringRef(Body).rtrim().ends_with("}"))
      Src += ";";
    Src += "\n}\n";
    Wrapped.push_back(I);
  }
  P.Source = Src;

  // Errors are wanted per statement rather than fatally, so diagnostics are
  // collected and attributed below instead of stopping the parse.
  //
  // Capturing them is what makes that attribution work at all. The default is
  // to capture nothing, so `stored_diag_begin()` was always empty and the
  // attribution below never fired, while the errors went to stderr as if the
  // user had asked to compile the synthesised source. A pattern's own errors
  // belong to the item they came from and nowhere else.
  std::unique_ptr<ASTUnit> Unit = tooling::buildASTFromCodeWithArgs(
      Src,
      {"-std=gnu11", "-w", "-ferror-limit=0", "-Wno-int-conversion",
       "-Wno-incompatible-pointer-types"},
      "spatch-pattern.c", "clang-spatch",
      std::make_shared<PCHContainerOperations>(),
      tooling::getClangStripDependencyFileAdjuster(),
      tooling::FileContentMappings(), /*DiagConsumer=*/nullptr,
      llvm::vfs::getRealFileSystem(), CaptureDiagsKind::All);
  if (!Unit)
    return;
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
    if (It->getLevel() < DiagnosticsEngine::Error ||
        isSynthesisArtefact(It->getID()))
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
      // Clang read the line and it left no statement behind, which is what a
      // type name written alone does: `Scsi_Cmnd;` declares nothing and only
      // warns. `parsePattern` reads this message as "synthesise it again as a
      // type", so an item reaching it is not the end of the story.
      P.Errors[Index] = ATypeNotAStatement.str();
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
        P.Errors[Index] = ATypeNotAStatement.str();
        continue;
      }
    // A pattern that is only a semicolon carries nothing to match.
    if (isa<NullStmt>(Only)) {
      P.Errors[Index] = "the pattern line carries no statement";
      continue;
    }
    if (AsGroup[Index]) {
      // The group's braces said which context to read its lines in, and what
      // the rule means is what stands between them. One element is that
      // element: `spatch` 1.1.1 rewrites the `.a = 7,` of
      // `{ .a = 7, .c = 8, }` and leaves the rest, matches one nested inside
      // another list, and matches one in a compound literal, which is what
      // searching for the element alone does. More than one needs them found
      // adjacent in the target's own list.
      const auto *DS = dyn_cast<DeclStmt>(Only);
      const auto *VD = DS && DS->isSingleDecl()
                           ? dyn_cast<VarDecl>(DS->getSingleDecl())
                           : nullptr;
      const auto *List =
          dyn_cast_or_null<InitListExpr>(peelRecovery(VD ? VD->getInit()
                                                         : nullptr));
      if (!List) {
        P.Errors[Index] = "the pattern names a brace group Clang could not "
                          "read as an initialiser list";
        continue;
      }
      if (List->getNumInits() != 1) {
        P.Errors[Index] = AGroupOfSeveralElements.str();
        continue;
      }
      P.Items[Index] = List->getInit(0);
      continue;
    }
    if (AsType[Index]) {
      // The declarator is only there to give the type a location, so what the
      // rule means is the type its pointee was written over.
      const auto *DS = dyn_cast<DeclStmt>(Only);
      const auto *VD = DS && DS->isSingleDecl()
                           ? dyn_cast<VarDecl>(DS->getSingleDecl())
                           : nullptr;
      const TypeSourceInfo *Info = VD ? VD->getTypeSourceInfo() : nullptr;
      const PointerTypeLoc PTL =
          Info ? Info->getTypeLoc().getAs<PointerTypeLoc>() : PointerTypeLoc();
      if (!PTL) {
        P.Errors[Index] = "the pattern names a type Clang could not read";
        continue;
      }
      P.TypeItems[Index] = PTL.getPointeeLoc();
    }
    P.Items[Index] = Only;
  }

  for (unsigned I : Wrapped)
    if (!P.Items[I] && P.Errors[I].empty())
      P.Errors[I] = "the pattern statement did not parse as C";
}

} // namespace

std::optional<ParsedPattern>
parsePattern(llvm::ArrayRef<MetaVar> MetaVars,
             llvm::ArrayRef<std::string> Statements,
             llvm::ArrayRef<std::string> TypeNames,
             llvm::ArrayRef<bool> BraceGroups, std::string &Error) {
  llvm::SmallVector<bool, 4> AsType(Statements.size(), false);
  llvm::SmallVector<bool, 4> AsGroup(Statements.size(), false);
  ParsedPattern P;
  parseOnce(MetaVars, Statements, TypeNames, AsType, AsGroup, P);
  if (!P.Unit) {
    Error = "Clang could not be run on the synthesised pattern";
    return std::nullopt;
  }

  // A statement that turned out to be a type is synthesised again with a
  // declarator over it, because that is the only way Clang gives the type a
  // location, and both the binding a metavariable holds and the range an edit
  // covers are locations. The second parse costs one more translation unit
  // and only for a patch that writes such a line.
  //
  // A brace group is synthesised again for the same reason. Its inner lines
  // are initialiser elements, which do not parse where a statement belongs,
  // so the group needs a declarator in front of it before Clang can read it.
  // Only the caller knows an item is a group, because the braces were joined
  // by grouping and the text alone does not say a `{` opened a construct
  // rather than a block.
  bool Again = false;
  for (unsigned I = 0, E = Statements.size(); I != E; ++I) {
    if (P.Errors[I] == ATypeNotAStatement) {
      AsType[I] = true;
      Again = true;
    } else if (!P.Errors[I].empty() && I < BraceGroups.size() &&
               BraceGroups[I]) {
      AsGroup[I] = true;
      Again = true;
    }
  }
  if (!Again)
    return P;

  ParsedPattern Typed;
  parseOnce(MetaVars, Statements, TypeNames, AsType, AsGroup, Typed);
  // The first pass read every other item, so it is what a caller gets when
  // the second cannot run at all. The type items are then reported as types
  // rather than as statements, which is what they are.
  if (!Typed.Unit)
    return P;
  // A declarator this synthesis invented must not reach a message a user
  // reads, and a failure here is a failure to read the type.
  for (unsigned I = 0, E = Statements.size(); I != E; ++I) {
    if (AsType[I] && !Typed.Errors[I].empty())
      Typed.Errors[I] = "the pattern names a type Clang could not read";
    // A group that the wrapper could not get Clang to read is reported as the
    // first pass read it, because that message names what the pattern wrote
    // and this one would name a declarator the synthesis invented. The count
    // of elements is this pass's own finding and says what the rule needs.
    if (AsGroup[I] && !Typed.Errors[I].empty() &&
        Typed.Errors[I] != AGroupOfSeveralElements)
      Typed.Errors[I] = P.Errors[I];
  }
  return Typed;
}

} // namespace clang::spatch

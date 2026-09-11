//===--- Unify.cpp - Match a pattern AST against a target AST ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Unify.h"
#include "Edit.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

namespace clang::spatch {

namespace {

/// The node both sides are compared at: parentheses and implicit casts are
/// written by the type rules, not by the author, so neither side is compared
/// through them.
const Stmt *peel(const Stmt *S) {
  if (const auto *E = dyn_cast_or_null<Expr>(S))
    return E->IgnoreParenImpCasts();
  return S;
}

/// The metavariable \p S refers to, or null when it is not a reference to one.
const MetaVar *metaVarOf(const Stmt *S, const ParsedPattern &Parsed) {
  const auto *Ref = dyn_cast_or_null<DeclRefExpr>(peel(S));
  if (!Ref)
    return nullptr;
  return Parsed.metaVarFor(Ref->getDecl()->getCanonicalDecl());
}

/// Is \p S the marker a `...` inside an argument list became?
bool isDotsMarker(const Stmt *S) {
  const auto *Call = dyn_cast_or_null<CallExpr>(peel(S));
  if (!Call)
    return false;
  const FunctionDecl *Callee = Call->getDirectCallee();
  return Callee && Callee->getName() == DotsMarker;
}

/// Can a metavariable of \p Kind stand for \p Target?
bool kindAccepts(enum MetaVar::Kind Kind, const Stmt *Target) {
  switch (Kind) {
  case MetaVar::Kind::Expression:
    return isa<Expr>(Target);
  case MetaVar::Kind::Constant:
    // A constant is a literal or an enumerator, which is what Coccinelle's own
    // expected output for tests/constx.cocci requires: it rewrites foo(12) and
    // foo('a') and leaves foo(x) alone.
    if (isa<IntegerLiteral, FloatingLiteral, StringLiteral, CharacterLiteral,
            CXXBoolLiteralExpr>(Target))
      return true;
    if (const auto *Ref = dyn_cast<DeclRefExpr>(Target))
      return isa<EnumConstantDecl>(Ref->getDecl());
    return false;
  case MetaVar::Kind::Identifier:
    return isa<DeclRefExpr, MemberExpr>(Target);
  case MetaVar::Kind::Statement:
    return true;
  case MetaVar::Kind::Type:
  case MetaVar::Kind::Position:
    // Neither stands for a subtree, so neither can be matched here. The parser
    // refuses a rule that uses one where a subtree is expected.
    return false;
  }
  return false;
}

/// Do two bindings name the same thing?
///
/// A declaration reference is compared by declaration, so `l` in one function
/// is not the `l` of another. Anything else is compared by the text the author
/// wrote, which is what Coccinelle compares, and it is also the only thing
/// available for a binding that is not a subtree at all.
bool sameBinding(const Binding &A, const Binding &B, ASTContext &Context) {
  const Stmt *PA = peel(A.Node), *PB = peel(B.Node);
  const auto *RA = dyn_cast_or_null<DeclRefExpr>(PA);
  const auto *RB = dyn_cast_or_null<DeclRefExpr>(PB);
  if (RA && RB)
    return RA->getDecl()->getCanonicalDecl() ==
           RB->getDecl()->getCanonicalDecl();
  if (RA || RB)
    return false;
  return sourceTextOf(A.Range, Context) == sourceTextOf(B.Range, Context);
}

/// Do two type specifiers, each read from its own translation unit, name the
/// same type?
///
/// This is the bottom of a declarator rather than the whole of a written
/// type: the paren, pointer, array and function layers above it have source
/// locations of their own and `Unifier::matchTypeLoc` walks those.
///
/// Written rather than canonical, because Coccinelle matches the spelling: a
/// pattern saying `long long` does not match a typedef that resolves to it.
/// A `QualType` cannot cross translation units, so nothing here compares one
/// by identity. A builtin compares by its kind and a named type by its name.
///
/// **The classes here, the declarator layers `matchTypeLoc` walks, and the
/// list in `uncomparableType` are one list.** A class none of the three
/// handles must be refused there, or a pattern would silently fail to match a
/// construct it describes.
bool sameWrittenType(QualType P, QualType T) {
  if (P.getLocalFastQualifiers() != T.getLocalFastQualifiers())
    return false;
  const Type *PT = P.getTypePtrOrNull(), *TT = T.getTypePtrOrNull();
  if (!PT || !TT)
    return PT == TT;
  if (PT->getTypeClass() != TT->getTypeClass())
    return false;
  if (const auto *PB = dyn_cast<BuiltinType>(PT))
    return PB->getKind() == cast<BuiltinType>(TT)->getKind();
  if (const auto *PC = dyn_cast<ComplexType>(PT))
    // `double _Complex` writes both halves in the one specifier, so the
    // element type has no location of its own to descend into.
    return sameWrittenType(PC->getElementType(),
                           cast<ComplexType>(TT)->getElementType());
  if (const auto *PD = dyn_cast<TypedefType>(PT))
    return PD->getDecl()->getName() ==
           cast<TypedefType>(TT)->getDecl()->getName();
  if (const auto *PR = dyn_cast<TagType>(PT)) {
    const TagDecl *PTag = PR->getDecl(), *TTag = cast<TagType>(TT)->getDecl();
    return PTag->getTagKind() == TTag->getTagKind() &&
           !PTag->getName().empty() && PTag->getName() == TTag->getName();
  }
  return false;
}

/// The type class in \p T that the comparison cannot handle, or an empty
/// string. See the note on `sameWrittenType`: the lists are one list.
std::string uncomparableType(QualType T) {
  const Type *P = T.getTypePtrOrNull();
  if (!P)
    return std::string();
  if (const auto *Paren = dyn_cast<ParenType>(P))
    return uncomparableType(Paren->getInnerType());
  if (isa<BuiltinType>(P))
    return std::string();
  if (const auto *Ptr = dyn_cast<PointerType>(P))
    return uncomparableType(Ptr->getPointeeType());
  if (const auto *C = dyn_cast<ComplexType>(P))
    return uncomparableType(C->getElementType());
  if (const auto *A = dyn_cast<ArrayType>(P)) {
    if (!isa<ConstantArrayType, IncompleteArrayType>(P))
      return std::string("an array of class ") + P->getTypeClassName();
    return uncomparableType(A->getElementType());
  }
  if (isa<TypedefType>(P))
    return std::string();
  if (const auto *Tag = dyn_cast<TagType>(P)) {
    // An anonymous tag has no name to compare, and comparing its members
    // instead would accept a different type that happens to agree.
    return Tag->getDecl()->getName().empty()
               ? std::string("an anonymous ") + P->getTypeClassName()
               : std::string();
  }
  if (const auto *F = dyn_cast<FunctionProtoType>(P)) {
    if (std::string Why = uncomparableType(F->getReturnType()); !Why.empty())
      return Why;
    for (unsigned I = 0, E = F->getNumParams(); I != E; ++I)
      if (std::string Why = uncomparableType(F->getParamType(I)); !Why.empty())
        return Why;
    return std::string();
  }
  if (const auto *F = dyn_cast<FunctionNoProtoType>(P))
    return uncomparableType(F->getReturnType());
  return std::string("a type of class ") + P->getTypeClassName();
}

/// The arguments of a call, with the dots markers separated out.
struct ArgPattern {
  llvm::SmallVector<const Expr *, 8> Terms; ///< Arguments that are not dots.
  bool LeadingDots = false;
  bool TrailingDots = false;
  bool InteriorDots = false; ///< Dots between two named terms.
};

ArgPattern splitArgs(const CallExpr &Call) {
  ArgPattern P;
  const unsigned N = Call.getNumArgs();
  llvm::SmallVector<bool, 8> IsDots(N, false);
  for (unsigned I = 0; I != N; ++I)
    IsDots[I] = isDotsMarker(Call.getArg(I));
  for (unsigned I = 0; I != N; ++I) {
    if (!IsDots[I]) {
      P.Terms.push_back(Call.getArg(I));
      continue;
    }
    if (I == 0)
      P.LeadingDots = true;
    else if (I == N - 1)
      P.TrailingDots = true;
    else
      P.InteriorDots = true;
  }
  return P;
}

/// The name \p RD's own declaration introduces, or an invalid range when that
/// declaration introduces anything besides the type.
///
/// This is what `T { int a; };` asks of a target, and what `T` binds to.
///
/// A tag written on its own is free standing and introduces its own name.
/// `TagType::isTagOwned` says a tag inside a declarator is owned by whatever
/// that declarator declares, so the declarations of the enclosing context that
/// own this one are the whole answer: exactly one, and a typedef.
SourceRange introducedTypeRange(const RecordDecl &RD) {
  if (!RD.isEmbeddedInDeclarator())
    return RD.getIdentifier() ? SourceRange(RD.getBeginLoc(), RD.getLocation())
                              : SourceRange();
  const Decl *Owner = nullptr;
  for (const Decl *D : RD.getDeclContext()->decls()) {
    const TypeSourceInfo *Info = nullptr;
    if (const auto *DD = dyn_cast<DeclaratorDecl>(D))
      Info = DD->getTypeSourceInfo();
    else if (const auto *TD = dyn_cast<TypedefNameDecl>(D))
      Info = TD->getTypeSourceInfo();
    if (!Info)
      continue;
    const auto *Tag = Info->getType()->getAs<TagType>();
    if (!Tag || !Tag->isTagOwned() ||
        Tag->getDecl()->getCanonicalDecl() != RD.getCanonicalDecl())
      continue;
    if (Owner)
      return SourceRange(); // Two names, as `typedef struct {} a, b;` writes.
    Owner = D;
  }
  const auto *TD = dyn_cast_or_null<TypedefNameDecl>(Owner);
  return TD ? SourceRange(TD->getLocation()) : SourceRange();
}

class Unifier {
public:
  Unifier(const ParsedPattern &Parsed, ASTContext &Context)
      : Parsed(Parsed), Context(Context),
        PatternContext(Parsed.Unit->getASTContext()) {}

  /// \p Out and \p TypeOut, when given, are appended with which target node
  /// each pattern node matched and which target type occurrence each written
  /// type matched. Neither is cleared, so both must be empty on the way in
  /// and the caller owns one pair of them per candidate.
  bool run(const Stmt *Pattern, const Stmt *Target, Bindings &Bound,
           NodePairs *Out = nullptr, TypeLocPairs *TypeOut = nullptr,
           DeclarationPairs *DeclOut = nullptr) {
    assert((!Out || Out->empty()) &&
           "run appends, so the caller owns one NodePairs per candidate");
    assert((!TypeOut || TypeOut->empty()) &&
           "run appends, so the caller owns one TypeLocPairs per candidate");
    assert((!DeclOut || DeclOut->empty()) &&
           "run appends, so the caller owns one DeclPairs per candidate");
    Pairs = Out;
    TypePairs = TypeOut;
    DeclPairs = DeclOut;
    const bool Matched = match(Pattern, Target, Bound);
    Pairs = nullptr;
    TypePairs = nullptr;
    DeclPairs = nullptr;
    return Matched;
  }

  /// Matches a type pattern against one written type occurrence.
  ///
  /// No correspondence is recorded, because a type pattern Clang can read is
  /// a type specifier and the occurrence it matches is the range to edit, so
  /// \c Match::Node already carries it.
  bool runOnType(TypeLoc Pattern, TypeLoc Target, Bindings &Bound) {
    Pairs = nullptr;
    TypePairs = nullptr;
    DeclPairs = nullptr;
    return matchTypeLoc(Pattern, Target, Bound);
  }

  /// Matches a declaration pattern against a declaration that is not inside
  /// any function body, so it has no `DeclStmt` to be compared through.
  ///
  /// \p Out and \p TypeOut are as in \c run.
  bool runOnDecl(const DeclStmt &Pattern, llvm::ArrayRef<const Decl *> Target,
                 Bindings &Bound, NodePairs *Out = nullptr,
                 TypeLocPairs *TypeOut = nullptr,
                 DeclarationPairs *DeclOut = nullptr) {
    assert((!Out || Out->empty()) && (!TypeOut || TypeOut->empty()) &&
           (!DeclOut || DeclOut->empty()) &&
           "runOnDecl appends, so the caller owns one of each per candidate");
    Pairs = Out;
    TypePairs = TypeOut;
    DeclPairs = DeclOut;
    const bool Matched = matchDecls(Pattern, Target, Bound);
    Pairs = nullptr;
    TypePairs = nullptr;
    DeclPairs = nullptr;
    return Matched;
  }

private:
  const ParsedPattern &Parsed;
  ASTContext &Context;        ///< The code's.
  ASTContext &PatternContext; ///< The pattern's, which is a separate TU.
  /// Where to record the pattern-node to target-node pairs, or null when the
  /// caller did not ask for them. Owned by \c run for the length of one
  /// candidate.
  NodePairs *Pairs = nullptr;
  /// Where to record the written-type correspondence, or null. Owned the same
  /// way \c Pairs is.
  TypeLocPairs *TypePairs = nullptr;
  /// Where to record the declaration correspondence, or null. Owned the same
  /// way \c Pairs is.
  DeclarationPairs *DeclPairs = nullptr;

  bool bind(const MetaVar &M, const Stmt *Target, Bindings &Bound) {
    if (!kindAccepts(M.Kind, Target))
      return false;
    return bindTo(M, Binding{Target, peel(Target)->getSourceRange()}, Bound);
  }

  /// Records \p B for \p M, or checks it against what \p M already holds.
  bool bindTo(const MetaVar &M, const Binding &B, Bindings &Bound) {
    auto It = Bound.find(M.Name);
    if (It != Bound.end())
      return sameBinding(It->second, B, Context);
    Bound[M.Name] = B;
    return true;
  }

  /// Matches a call's arguments against a pattern's, honouring `...`.
  bool matchArgs(const CallExpr &Pattern, const CallExpr &Target,
                 Bindings &Bound) {
    const ArgPattern P = splitArgs(Pattern);
    const unsigned N = Target.getNumArgs();
    // A named term after a leading `...` sits at no fixed position, so it is
    // tried at every position it could occupy.
    if (P.InteriorDots)
      return false; // `f(A, ..., B)` needs the last position; not built.
    if (!P.LeadingDots && !P.TrailingDots)
      return P.Terms.size() == N && matchRun(P.Terms, Target, 0, Bound);
    if (!P.LeadingDots) {
      // Prefix: the named terms are at the front and the rest is free.
      return P.Terms.size() <= N && matchRun(P.Terms, Target, 0, Bound);
    }
    if (P.Terms.empty())
      return true; // `f(...)` accepts any arguments at all.
    if (!P.TrailingDots) {
      // Suffix: the named terms end the list.
      return P.Terms.size() <= N &&
             matchRun(P.Terms, Target, N - P.Terms.size(), Bound);
    }
    // Surrounded: try every window the terms could occupy, and keep the
    // bindings of the first that matches so the caller sees one match.
    for (unsigned Start = 0; Start + P.Terms.size() <= N; ++Start) {
      Bindings Trial = Bound;
      const size_t Mark = Pairs ? Pairs->size() : 0;
      if (matchRun(P.Terms, Target, Start, Trial)) {
        Bound = std::move(Trial);
        return true;
      }
      // A window is the one failure inside a match that the caller never
      // sees, because a later window can still match, so it has to undo its
      // own pairs.
      if (Pairs)
        Pairs->resize(Mark);
    }
    return false;
  }

  bool matchRun(llvm::ArrayRef<const Expr *> Terms, const CallExpr &Target,
                unsigned Start, Bindings &Bound) {
    for (unsigned I = 0; I != Terms.size(); ++I)
      if (!match(Terms[I], Target.getArg(Start + I), Bound))
        return false;
    return true;
  }

  /// Does one declaration of the pattern match one of the target?
  ///
  /// The written type, the declared name and the initialiser are compared,
  /// and none of the three is a child of the declaration, so comparing class
  /// plus children made every childless declaration match every other one.
  ///
  /// A record member goes through here too. A \c FieldDecl is a
  /// \c DeclaratorDecl, so its written type and its name are compared the way
  /// a variable's are, and the bit-field width is the one thing it can carry
  /// that a variable cannot.
  bool matchOneDecl(const Decl *PD, const Decl *TD, Bindings &Bound) {
    // Recorded before anything is compared, so a rule marking a member names
    // the range the target wrote it over. A member is neither a `Stmt` nor a
    // written type, so this is the only correspondence that can carry it.
    if (DeclPairs)
      DeclPairs->push_back({PD, TD});
    const auto *PDecl = dyn_cast<DeclaratorDecl>(PD);
    const auto *TDecl = dyn_cast<DeclaratorDecl>(TD);
    const auto *PT = dyn_cast<TypedefNameDecl>(PD);
    const auto *TT = dyn_cast<TypedefNameDecl>(TD);
    const auto *PR = dyn_cast<RecordDecl>(PD);
    const auto *TR = dyn_cast<RecordDecl>(TD);
    if (PDecl && TDecl) {
      if (!matchWrittenType(PDecl->getTypeSourceInfo(),
                            TDecl->getTypeSourceInfo(), Bound) ||
          !matchDeclaredName(*PDecl, *TDecl, Bound))
        return false;
      const auto *PF = dyn_cast<FieldDecl>(PDecl);
      const auto *TF = dyn_cast<FieldDecl>(TDecl);
      if (PF && TF) {
        if (PF->isBitField() != TF->isBitField())
          return false;
        return !PF->isBitField() ||
               match(PF->getBitWidth(), TF->getBitWidth(), Bound);
      }
      const auto *PV = dyn_cast<VarDecl>(PDecl);
      const auto *TV = dyn_cast<VarDecl>(TDecl);
      if (!PV != !TV)
        return false;
      return !PV || match(PV->getInit(), TV->getInit(), Bound);
    }
    if (PR && TR)
      return matchRecord(*PR, *TR, Bound);
    if (!PT || !TT)
      return false;
    return matchWrittenType(PT->getTypeSourceInfo(), TT->getTypeSourceInfo(),
                            Bound) &&
           matchTypedefName(*PT, *TT, Bound);
  }

  /// Does a record definition written in the pattern match one in the target?
  ///
  /// The member list is compared element for element with nothing implicit
  /// between the members. `spatch` 1.1.1 refuses `T { int a; };` against
  /// `struct foo {int a; int c;};` and against
  /// `typedef struct blah {int a; int c;} name;`, and writing `...` inside the
  /// braces is what asks for the loose reading. So a record body is unlike an
  /// initialiser list, where one element matches inside a longer one.
  bool matchRecord(const RecordDecl &Pattern, const RecordDecl &Target,
                   Bindings &Bound) {
    if (!Pattern.isCompleteDefinition() || !Target.isCompleteDefinition())
      return false;
    // A tag that stands for a type metavariable constrains neither the kind
    // nor the name, so `T { int a; };` matches a `union` as readily as a
    // `struct`. What it does require is that the declaration introduce the
    // type and nothing else, which is what the binding's own range says.
    if (const MetaVar *M = Parsed.metaVarFor(Pattern.getCanonicalDecl())) {
      const SourceRange Introduced = introducedTypeRange(Target);
      if (!Introduced.isValid() ||
          !bindTo(*M, Binding{nullptr, Introduced}, Bound))
        return false;
      return matchMembers(Pattern, Target, Bound);
    }
    if (Pattern.getTagKind() != Target.getTagKind())
      return false;
    // The tag the patch wrote, with "no tag" a spelling of its own. `spatch`
    // 1.1.1 leaves `struct bar {int a;};` alone for a pattern writing
    // `struct foo`, leaves `struct foo { int a; } s;` alone for one writing
    // `struct { int a; } s;`, and rewrites the anonymous target for that same
    // pattern. Comparing the members alone rewrote `struct bar` from a
    // pattern naming `struct foo`.
    const IdentifierInfo *PN = Pattern.getIdentifier();
    const IdentifierInfo *TN = Target.getIdentifier();
    if (!PN != !TN)
      return false;
    if (PN && PN->getName() != TN->getName())
      return false;
    return matchMembers(Pattern, Target, Bound);
  }

  /// Do the two member lists agree, member for member?
  bool matchMembers(const RecordDecl &Pattern, const RecordDecl &Target,
                    Bindings &Bound) {
    auto PIt = Pattern.decls_begin(), PEnd = Pattern.decls_end();
    auto TIt = Target.decls_begin(), TEnd = Target.decls_end();
    for (; PIt != PEnd && TIt != TEnd; ++PIt, ++TIt)
      if (!matchOneDecl(*PIt, *TIt, Bound))
        return false;
    return PIt == PEnd && TIt == TEnd;
  }

  /// Does a declaration statement of the pattern match one of the target,
  /// declarator by declarator?
  bool matchDecls(const DeclStmt &Pattern, llvm::ArrayRef<const Decl *> Target,
                  Bindings &Bound) {
    // A pattern that is one record definition is compared against the record
    // of the target's group, and whether the rest of that group is acceptable
    // is `introducedTypeRange`'s question. Comparing the groups element for
    // element instead left `typedef struct blah {int a;} name;` unmatched
    // inside a function body, where both declarations share one `DeclStmt`,
    // while the same declaration at file scope matched: there the record is
    // offered on its own.
    if (Pattern.isSingleDecl())
      if (const auto *PR = dyn_cast<RecordDecl>(Pattern.getSingleDecl()))
        for (const Decl *D : Target)
          if (const auto *TR = dyn_cast<RecordDecl>(D))
            return matchOneDecl(PR, TR, Bound);
    auto PIt = Pattern.decl_begin(), PEnd = Pattern.decl_end();
    auto TIt = Target.begin(), TEnd = Target.end();
    for (; PIt != PEnd && TIt != TEnd; ++PIt, ++TIt)
      if (!matchOneDecl(*PIt, *TIt, Bound))
        return false;
    return PIt == PEnd && TIt == TEnd;
  }

  /// Matches a type written in the pattern against the one written in the
  /// target, binding a type metavariable to whatever the target wrote.
  bool matchWrittenType(const TypeSourceInfo *Pattern,
                        const TypeSourceInfo *Target, Bindings &Bound) {
    if (!Pattern || !Target)
      return false;
    return matchTypeLoc(Pattern->getTypeLoc(), Target->getTypeLoc(), Bound);
  }

  /// Matches one written type occurrence against another, descending through
  /// the declarator layers that have a location of their own.
  ///
  /// Over \c TypeLoc rather than \c QualType, because a metavariable binding
  /// and an edit both need a source range and a \c QualType carries none. A
  /// layer with no location structure of its own is decided by
  /// \c sameWrittenType.
  bool matchTypeLoc(TypeLoc Pattern, TypeLoc Target, Bindings &Bound) {
    // A null loc is what Clang leaves where nothing was written, so recursing
    // on one would compare a handle to no type at all.
    if (Pattern.isNull() || Target.isNull())
      return Pattern.isNull() == Target.isNull();
    TypeLoc P = Pattern.getUnqualifiedLoc(), T = Target.getUnqualifiedLoc();
    // A parenthesised declarator has to be written on both sides. Measured on
    // `spatch` 1.1.1, `- int *` does not match `int (*p);`, and matching it
    // would also give the edit a range covering the name being declared,
    // because a paren layer runs to its own closing parenthesis.
    const ParenTypeLoc PP = P.getAs<ParenTypeLoc>();
    const ParenTypeLoc TP = T.getAs<ParenTypeLoc>();
    if (PP || TP) {
      if (!PP || !TP)
        return false;
      return matchTypeLoc(PP.getInnerLoc(), TP.getInnerLoc(), Bound);
    }
    // Recorded before the qualifiers come off, so a pattern marking
    // `const int` names the range the target wrote them over.
    if (TypePairs)
      TypePairs->push_back({Pattern, Target});
    // A type metavariable stands for whatever the target wrote, qualifiers
    // included, so it is tried before they are compared. A pattern writing a
    // qualifier of its own still requires it: `- const T x;` leaves an
    // unqualified declaration alone and binds `int` from `const int`, while
    // `- T x;` binds the whole of `const int`. Both were read off `spatch`
    // 1.1.1.
    const unsigned PQuals = Pattern.getType().getLocalFastQualifiers();
    const unsigned TQuals = Target.getType().getLocalFastQualifiers();
    if (const MetaVar *M = typeMetaVarOf(P.getType())) {
      if ((TQuals & PQuals) != PQuals)
        return false;
      return bindTo(*M,
                    Binding{nullptr, Target.getSourceRange(),
                            /*RangeIsShort=*/TQuals != PQuals},
                    Bound);
    }
    if (PQuals != TQuals)
      return false;
    if (P.getTypeLocClass() != T.getTypeLocClass())
      return false;
    if (PointerTypeLoc PPtr = P.getAs<PointerTypeLoc>())
      return matchTypeLoc(PPtr.getPointeeLoc(),
                          T.castAs<PointerTypeLoc>().getPointeeLoc(), Bound);
    if (ArrayTypeLoc PArr = P.getAs<ArrayTypeLoc>()) {
      // Kept in step with `uncomparableType`, whose class list is this one:
      // an array class it refuses cannot reach here, and this says so.
      if (!isa<ConstantArrayType, IncompleteArrayType>(P.getTypePtr()))
        return false;
      // The class test above already agreed on which array kind this is, so a
      // constant one has a size on both sides.
      if (const auto *PC = dyn_cast<ConstantArrayType>(P.getTypePtr()))
        if (PC->getSize() != cast<ConstantArrayType>(T.getTypePtr())->getSize())
          return false;
      return matchTypeLoc(PArr.getElementLoc(),
                          T.castAs<ArrayTypeLoc>().getElementLoc(), Bound);
    }
    if (FunctionTypeLoc PFn = P.getAs<FunctionTypeLoc>()) {
      FunctionTypeLoc TFn = T.castAs<FunctionTypeLoc>();
      if (const auto *PProto = dyn_cast<FunctionProtoType>(P.getTypePtr()))
        if (PProto->isVariadic() !=
            cast<FunctionProtoType>(T.getTypePtr())->isVariadic())
          return false;
      if (PFn.getNumParams() != TFn.getNumParams())
        return false;
      if (!matchTypeLoc(PFn.getReturnLoc(), TFn.getReturnLoc(), Bound))
        return false;
      // A parameter is compared as the declaration wrote it and not as the
      // function type holds it, because one of array type decays in the type
      // and keeps its brackets in the source.
      for (unsigned I = 0, E = PFn.getNumParams(); I != E; ++I) {
        const ParmVarDecl *PV = PFn.getParam(I), *TV = TFn.getParam(I);
        if (!PV || !TV)
          return false;
        if (!matchWrittenType(PV->getTypeSourceInfo(), TV->getTypeSourceInfo(),
                              Bound))
          return false;
      }
      return true;
    }
    return sameWrittenType(P.getType(), T.getType());
  }

  /// Matches the name \p Pattern declares against \p Target's, binding an
  /// identifier metavariable to whatever the target called it.
  bool matchDeclaredName(const NamedDecl &Pattern, const NamedDecl &Target,
                         Bindings &Bound) {
    const MetaVar *M = Parsed.metaVarNamed(Pattern.getName());
    if (!M || M->Kind != MetaVar::Kind::Identifier)
      return Pattern.getName() == Target.getName();
    return bindTo(*M, Binding{nullptr, Target.getLocation()}, Bound);
  }

  /// Matches the name a `typedef` pattern introduces against the one the
  /// target introduces.
  ///
  /// This is the one position where a `type` metavariable stands for a name
  /// being declared rather than for a type written out. `type t, s;` over
  /// `typedef t s;` binds `t` to what the target aliased and `s` to the alias
  /// it gave it.
  bool matchTypedefName(const TypedefNameDecl &Pattern,
                        const TypedefNameDecl &Target, Bindings &Bound) {
    const MetaVar *M = Parsed.metaVarNamed(Pattern.getName());
    if (M && M->Kind == MetaVar::Kind::Type)
      return bindTo(*M, Binding{nullptr, Target.getLocation()}, Bound);
    return matchDeclaredName(Pattern, Target, Bound);
  }

  /// The type metavariable \p T is, or null. `type T;` is synthesised as
  /// `typedef int T;`, so a pattern naming it carries that typedef.
  const MetaVar *typeMetaVarOf(QualType T) const {
    const auto *TD = dyn_cast_or_null<TypedefType>(T.getTypePtrOrNull());
    if (!TD)
      return nullptr;
    const MetaVar *M = Parsed.metaVarFor(TD->getDecl()->getCanonicalDecl());
    return M && M->Kind == MetaVar::Kind::Type ? M : nullptr;
  }

  /// Do the two designations name the same fields and indices, in order?
  ///
  /// A designator is compared as the patch wrote it. The pattern's array
  /// wrapper cannot resolve a field designator, so its own name is the
  /// identifier the parser recorded while the target's is the field Sema
  /// found, and `getFieldName` gives the identifier on both sides. `spatch`
  /// 1.1.1 refuses `.a` against `.b` and refuses `[0]` against `.a`, so the
  /// kind and the name both count. An index is compared by its written text
  /// for the same reason a literal is.
  bool matchDesignators(const DesignatedInitExpr &P,
                        const DesignatedInitExpr &T) {
    if (P.size() != T.size())
      return false;
    for (unsigned I = 0, E = P.size(); I != E; ++I) {
      const DesignatedInitExpr::Designator &PD = *P.getDesignator(I);
      const DesignatedInitExpr::Designator &TD = *T.getDesignator(I);
      if (PD.isFieldDesignator() != TD.isFieldDesignator() ||
          PD.isArrayDesignator() != TD.isArrayDesignator())
        return false;
      if (PD.isFieldDesignator()) {
        // By name and not by identifier. Each side has its own
        // `IdentifierTable`, so two spellings that are one name inside one
        // translation unit are two pointers across the pair, which is the
        // same trap the literal comparison below records for `SourceManager`.
        const IdentifierInfo *PN = PD.getFieldName(), *TN = TD.getFieldName();
        if (!PN || !TN || PN->getName() != TN->getName())
          return false;
        continue;
      }
      // An array or array-range designator holds its bounds as expressions of
      // the list rather than as children of the designator.
      const Expr *PLo = P.getArrayIndex(PD), *TLo = T.getArrayIndex(TD);
      if (!PLo || !TLo ||
          sourceTextOf(*PLo, PatternContext) != sourceTextOf(*TLo, Context))
        return false;
      if (PD.isArrayRangeDesignator() != TD.isArrayRangeDesignator())
        return false;
    }
    return true;
  }

  bool match(const Stmt *Pattern, const Stmt *Target, Bindings &Bound) {
    if (!Pattern || !Target)
      return Pattern == Target;
    // Recorded before peeling, so a pattern written `(i = i2)` pairs with the
    // target's own parentheses and an edit on it takes them too.
    if (Pairs)
      Pairs->push_back({Pattern, Target});
    const Stmt *P = peel(Pattern);
    const Stmt *T = peel(Target);
    if (!P || !T)
      return P == T;

    if (const MetaVar *M = metaVarOf(P, Parsed))
      return bind(*M, T, Bound);

    if (P->getStmtClass() != T->getStmtClass())
      return false;

    // Node kinds whose identity is more than their children.
    if (const auto *PB = dyn_cast<BinaryOperator>(P)) {
      const auto *TB = cast<BinaryOperator>(T);
      if (PB->getOpcode() != TB->getOpcode())
        return false;
    } else if (const auto *PU = dyn_cast<UnaryOperator>(P)) {
      const auto *TU = cast<UnaryOperator>(T);
      if (PU->getOpcode() != TU->getOpcode())
        return false;
    } else if (const auto *PR = dyn_cast<DeclRefExpr>(P)) {
      const auto *TR = cast<DeclRefExpr>(T);
      return PR->getDecl()->getName() == TR->getDecl()->getName();
    } else if (const auto *PM = dyn_cast<MemberExpr>(P)) {
      const auto *TM = cast<MemberExpr>(T);
      if (PM->getMemberDecl()->getName() != TM->getMemberDecl()->getName() ||
          PM->isArrow() != TM->isArrow())
        return false;
      return match(PM->getBase(), TM->getBase(), Bound);
    } else if (const auto *PC = dyn_cast<CallExpr>(P)) {
      const auto *TC = cast<CallExpr>(T);
      if (!match(PC->getCallee(), TC->getCallee(), Bound))
        return false;
      return matchArgs(*PC, *TC, Bound);
    } else if (const auto *PD = dyn_cast<DeclStmt>(P)) {
      const auto *TD = cast<DeclStmt>(T);
      const llvm::SmallVector<const Decl *, 4> Decls(TD->decls());
      return matchDecls(*PD, Decls, Bound);
    } else if (const auto *PCast = dyn_cast<CStyleCastExpr>(P)) {
      const auto *TCast = cast<CStyleCastExpr>(T);
      return matchWrittenType(PCast->getTypeInfoAsWritten(),
                              TCast->getTypeInfoAsWritten(), Bound) &&
             match(PCast->getSubExpr(), TCast->getSubExpr(), Bound);
    } else if (const auto *PSize = dyn_cast<UnaryExprOrTypeTraitExpr>(P)) {
      const auto *TSize = cast<UnaryExprOrTypeTraitExpr>(T);
      // `sizeof` and `_Alignof` are the same class, and the operand is a
      // child only when it is an expression.
      if (PSize->getKind() != TSize->getKind() ||
          PSize->isArgumentType() != TSize->isArgumentType())
        return false;
      if (PSize->isArgumentType())
        return matchWrittenType(PSize->getArgumentTypeInfo(),
                                TSize->getArgumentTypeInfo(), Bound);
      return match(PSize->getArgumentExpr(), TSize->getArgumentExpr(), Bound);
    } else if (const auto *PDes = dyn_cast<DesignatedInitExpr>(P)) {
      const auto *TDes = cast<DesignatedInitExpr>(T);
      if (!matchDesignators(*PDes, *TDes))
        return false;
      return match(PDes->getInit(), TDes->getInit(), Bound);
    } else if (isa<IntegerLiteral, FloatingLiteral, CharacterLiteral,
                   StringLiteral>(P)) {
      // Each side is read with its own SourceManager. Reading the pattern's
      // location in the code's manager gives an unrelated offset, and every
      // literal comparison then failed.
      return sourceTextOf(*P, PatternContext) == sourceTextOf(*T, Context);
    }

    // Everything else is its class plus its children in order. That covers
    // every statement form Clang can parse without naming it here.
    auto PIt = P->children().begin(), PEnd = P->children().end();
    auto TIt = T->children().begin(), TEnd = T->children().end();
    for (; PIt != PEnd && TIt != TEnd; ++PIt, ++TIt)
      if (!match(*PIt, *TIt, Bound))
        return false;
    return PIt == PEnd && TIt == TEnd;
  }
};

/// Is \p S's identity fully given by its class and its children in order?
bool isComparableByStructure(const Stmt &S) {
  return isa<CompoundStmt, IfStmt, WhileStmt, DoStmt, ForStmt, ReturnStmt,
             BreakStmt, ContinueStmt, NullStmt, SwitchStmt, CaseStmt,
             DefaultStmt, ParenExpr, ArraySubscriptExpr, InitListExpr, StmtExpr,
             AbstractConditionalOperator>(&S);
}

/// The classes `Unifier::match` decides by more than class and children.
bool isComparedExplicitly(const Stmt &S) {
  return isa<BinaryOperator, UnaryOperator, DeclRefExpr, MemberExpr, CallExpr,
             DeclStmt, CStyleCastExpr, UnaryExprOrTypeTraitExpr, IntegerLiteral,
             FloatingLiteral, CharacterLiteral, StringLiteral,
             DesignatedInitExpr>(&S);
}

/// Why a declaration pattern cannot be compared, or an empty string.
///
/// A declarator that is not a variable has no counterpart in the comparison,
/// and a type whose class `sameWrittenType` does not handle would fail to
/// match a declaration it describes.
std::string whyNotComparableType(const TypeSourceInfo *Info) {
  if (!Info)
    return "a type that was not written out";
  return uncomparableType(Info->getType());
}

/// Why the unifier cannot compare a declaration \p D of a pattern, or an
/// empty string. A record definition is answered through its members, each of
/// which is a declaration in its own right.
std::string whyNotComparableOneDecl(const Decl *D) {
  // A record definition is compared through its members, each of which is a
  // declaration in its own right.
  if (const auto *RD = dyn_cast<RecordDecl>(D)) {
    if (!RD->isCompleteDefinition())
      return "the pattern names a record without defining it, so there is no "
             "member list to match";
    for (const Decl *Member : RD->decls())
      if (std::string Why = whyNotComparableOneDecl(Member); !Why.empty())
        return Why;
    return std::string();
  }
  const TypeSourceInfo *Info = nullptr;
  if (const auto *DD = dyn_cast<DeclaratorDecl>(D))
    Info = DD->getTypeSourceInfo();
  else if (const auto *TD = dyn_cast<TypedefNameDecl>(D))
    Info = TD->getTypeSourceInfo();
  else
    return std::string("the pattern's declaration holds a ") +
           D->getDeclKindName() +
           " declarator, which the comparison has no counterpart for";
  if (std::string Why = whyNotComparableType(Info); !Why.empty())
    return "the pattern declares " + Why +
           ", which the type comparison does not handle";
  return std::string();
}

std::string whyNotComparableInDecls(const DeclStmt &S) {
  for (const Decl *D : S.decls())
    if (std::string Why = whyNotComparableOneDecl(D); !Why.empty())
      return Why;
  return std::string();
}


/// Every declaration written at file scope, and by default only those that
/// declare exactly one thing.
///
/// There is no `DeclStmt` outside a function body, so a declaration there is
/// reachable only from the translation unit's own declaration list.
///
/// A declaration with more than one declarator is left out unless
/// \p MultiDeclaratorOK. Its declarators share one `;`, so replacing one of
/// them would take the terminator the others need. That bars rewriting one
/// and does not bar reading a binding off one, so `typedef int A, B;` can
/// still tell a rule that asks for no change what `A` and `B` alias.
llvm::SmallVector<const Decl *, 8>
fileScopeDeclarations(ASTContext &Context, bool MultiDeclaratorOK) {
  llvm::SmallVector<const Decl *, 8> Out;
  llvm::DenseMap<const void *, unsigned> PerDeclaration;
  const TranslationUnitDecl *TU = Context.getTranslationUnitDecl();
  // Clang puts its own predefined typedefs, `__builtin_va_list` among them,
  // at the head of every translation unit with no source location at all. No
  // patch can name one, and a match on one has nowhere to be reported.
  const auto Eligible = [&](const Decl *D) {
    if (D->isImplicit() || D->getBeginLoc().isInvalid())
      return false;
    if (const auto *RD = dyn_cast<RecordDecl>(D))
      return RD->isCompleteDefinition() &&
             introducedTypeRange(*RD).isValid();
    return isa<DeclaratorDecl, TypedefNameDecl>(D) && !isa<FunctionDecl>(D);
  };
  // Clang gives every declarator of one declaration the same begin location,
  // which is what separates `int a, b;` from `int a; int b;`.
  for (const Decl *D : TU->decls())
    if (Eligible(D))
      ++PerDeclaration[D->getBeginLoc().getPtrEncoding()];
  for (const Decl *D : TU->decls())
    if (Eligible(D) && (MultiDeclaratorOK ||
                        PerDeclaration[D->getBeginLoc().getPtrEncoding()] == 1))
      Out.push_back(D);
  return Out;
}

/// Collects every statement of a translation unit, outermost first.
class StmtCollector : public RecursiveASTVisitor<StmtCollector> {
public:
  bool VisitStmt(Stmt *S) {
    All.push_back(S);
    return true;
  }
  std::vector<Stmt *> All;
};

/// Collects every written type occurrence of a translation unit, outermost
/// first.
class TypeLocCollector : public RecursiveASTVisitor<TypeLocCollector> {
public:
  bool VisitTypeLoc(TypeLoc TL) {
    // Clang's own predefined typedefs head every translation unit with no
    // source location at all, and a match on one has nowhere to be reported.
    if (TL.getSourceRange().getBegin().isValid())
      All.push_back(TL);
    return true;
  }
  std::vector<TypeLoc> All;
};

/// \c whyNotComparable for a node reached from the `-` side, rather than for
/// the `-` side itself.
///
/// The two differ for a block. Which blocks a block pattern may take is a
/// question about the whole `-` side, while a block written inside a larger
/// pattern is the body of the construct above it and is compared as one of its
/// children.
std::string whyNotComparableNode(const Stmt *Pattern) {
  if (!Pattern)
    return std::string();
  const Stmt *P = peel(Pattern);
  if (!P)
    return std::string();
  if (!isComparedExplicitly(*P) && !isComparableByStructure(*P)) {
    // Clang error-recovers, so a tree can come back from text it could not
    // read, and the recovery node is what is left of a diagnostic the pattern
    // parser's wrapper-line attribution missed.
    if (isa<RecoveryExpr>(P))
      return "the pattern did not parse as C, and Clang recovered rather "
             "than refusing it";
    return std::string("the pattern holds a ") + P->getStmtClassName() +
           ", and the unifier decides that class by its children alone, so "
           "it would match a construct the rule does not describe";
  }
  if (const auto *DS = dyn_cast<DeclStmt>(P))
    if (std::string Why = whyNotComparableInDecls(*DS); !Why.empty())
      return Why;
  if (const auto *C = dyn_cast<CStyleCastExpr>(P))
    if (std::string Why = whyNotComparableType(C->getTypeInfoAsWritten());
        !Why.empty())
      return "the pattern casts to " + Why +
             ", which the type comparison does not handle";
  if (const auto *Sz = dyn_cast<UnaryExprOrTypeTraitExpr>(P))
    if (Sz->isArgumentType())
      if (std::string Why = whyNotComparableType(Sz->getArgumentTypeInfo());
          !Why.empty())
        return "the pattern names " + Why +
               ", which the type comparison does not handle";
  for (const Stmt *Child : P->children())
    if (std::string Why = whyNotComparableNode(Child); !Why.empty())
      return Why;
  return std::string();
}

} // namespace

std::string whyNotComparable(const Stmt *Pattern) {
  // A `-` side that is a block matches where Coccinelle's own reading says it
  // does not. `spatch` 1.1.1 leaves a function's own body alone for `- {` over
  // `  foo();` over `- }`, and changes nothing at all when the same block is
  // written on one line, while the walk here offers every `CompoundStmt`
  // including a function body: `- { foo(); }` turned `int main() { foo(); }`
  // into `int main() foo();` at exit 0. Which blocks a block pattern may take
  // is the piece that is missing, so it is named rather than approximated.
  if (isa_and_present<CompoundStmt>(peel(Pattern)))
    return "the `-` side is a block, and which blocks a block pattern may "
           "take is decided by more than the block itself: Coccinelle leaves "
           "a function's own body alone, which this version does not express";
  return whyNotComparableNode(Pattern);
}

bool unify(const Stmt *Pattern, const Stmt *Target, const ParsedPattern &Parsed,
           ASTContext &Context, Bindings &Bound) {
  return Unifier(Parsed, Context).run(Pattern, Target, Bound);
}

std::string bindingKey(const Binding &B, ASTContext &Context) {
  if (const auto *Ref = dyn_cast_or_null<DeclRefExpr>(peel(B.Node)))
    return "decl:" + llvm::utohexstr(reinterpret_cast<uintptr_t>(
                         Ref->getDecl()->getCanonicalDecl()));
  return "text:" + sourceTextOf(B.Range, Context).str();
}

std::vector<Match> findTypeMatches(TypeLoc Pattern, const ParsedPattern &Parsed,
                                   ASTContext &Context, MatchOptions Opts) {
  std::vector<Match> Out;
  if (Pattern.isNull())
    return Out;
  TypeLocCollector Collector;
  Collector.TraverseDecl(Context.getTranslationUnitDecl());
  Unifier Shared(Parsed, Context);
  const Bindings Seed = Opts.Inherited ? *Opts.Inherited : Bindings();
  for (TypeLoc TL : Collector.All) {
    Bindings Bound = Seed;
    if (!Shared.runOnType(Pattern, TL, Bound))
      continue;
    Out.push_back(
        {DynTypedNode::create(TL), std::move(Bound), 0, {}, {}, {}});
  }
  return Out;
}

std::string whyNotATypePattern(TypeLoc Pattern, const ParsedPattern &Parsed) {
  const auto *TD =
      dyn_cast_or_null<TypedefType>(Pattern.getType().getTypePtrOrNull());
  const MetaVar *M =
      TD ? Parsed.metaVarFor(TD->getDecl()->getCanonicalDecl()) : nullptr;
  if (M && M->Kind == MetaVar::Kind::Type)
    return "the `-` side is a type metavariable written on its own, so it "
           "names the whole written type of every declaration in the file, "
           "and Coccinelle reprints each of those declarations rather than "
           "writing the new type where the old one stood";
  return std::string();
}

std::vector<Match> findMatches(const Stmt *Pattern, const ParsedPattern &Parsed,
                               ASTContext &Context, MatchOptions Opts) {
  return findMatches(llvm::ArrayRef<const Stmt *>(Pattern), Parsed, Context,
                     Opts);
}

std::vector<Match> findMatches(llvm::ArrayRef<const Stmt *> Patterns,
                               const ParsedPattern &Parsed, ASTContext &Context,
                               MatchOptions Opts) {
  std::vector<Match> Out;
  if (llvm::all_of(Patterns, [](const Stmt *P) { return !P; }))
    return Out;
  assert((Opts.MayNest.empty() || Opts.MayNest.size() == Patterns.size()) &&
         "MayNest is read per pattern, so a short one would silently stop the "
         "patterns past its end from nesting");
  StmtCollector Collector;
  Collector.TraverseDecl(Context.getTranslationUnitDecl());
  Unifier Shared(Parsed, Context);
  const Bindings Seed = Opts.Inherited ? *Opts.Inherited : Bindings();
  const llvm::SmallVector<const Decl *, 8> FileScope =
      fileScopeDeclarations(Context, Opts.MultiDeclaratorOK);

  // What earlier matches already cover. A pattern is not offered a node whose
  // text any of them took, which for one pattern is the subtree exclusion
  // that reports a written call once rather than again through its own
  // argument, and across patterns is what makes an earlier branch win.
  //
  // Statements and file-scope declarations are recorded apart, because they
  // exclude each other asymmetrically. Among declarations the test has to be
  // identity: Clang gives every declarator of one declaration the same begin
  // location, so `typedef int A, B, C, D;` is four declarations over one range
  // and a range test lets the first hide the other three. Against statements
  // the test has to be range, because a file-scope initialiser's
  // subexpressions are in the statement walk, so a branch matching `1 + 2`
  // and a branch matching `int x = 1 + 2;` otherwise both fire on the same
  // text.
  llvm::SmallVector<SourceRange, 8> StmtRanges;
  llvm::SmallVector<SourceRange, 8> DeclRanges;
  llvm::DenseSet<const Decl *> ClaimedDecls;
  const auto overlaps = [](llvm::ArrayRef<SourceRange> Ranges, SourceRange R) {
    return llvm::any_of(Ranges, [&](SourceRange C) {
      return R.getBegin() <= C.getEnd() && C.getBegin() <= R.getEnd();
    });
  };

  for (unsigned P = 0, PE = Patterns.size(); P != PE; ++P) {
    if (!Patterns[P])
      continue;
    // A pattern that may nest is still kept out of what an earlier pattern
    // took, so only the ranges this pattern adds are exempt.
    const bool Nests = P < Opts.MayNest.size() && Opts.MayNest[P];
    const size_t Earlier = StmtRanges.size();
    for (Stmt *S : Collector.All) {
      const SourceRange R = S->getSourceRange();
      const llvm::ArrayRef<SourceRange> Blocked =
          Nests ? llvm::ArrayRef<SourceRange>(StmtRanges).take_front(Earlier)
                : llvm::ArrayRef<SourceRange>(StmtRanges);
      if (overlaps(Blocked, R) || overlaps(DeclRanges, R))
        continue;
      Bindings Bound = Seed;
      // Fresh for each candidate. A candidate that fails is compared node by
      // node and gets some way in before it fails, so a vector reused down
      // the list would report every near miss as the match's own.
      NodePairs Pairs;
      TypeLocPairs TypePairs;
      DeclarationPairs DeclPairs;
      if (!Shared.run(Patterns[P], S, Bound,
                      Opts.WantPairs ? &Pairs : nullptr,
                      Opts.WantPairs ? &TypePairs : nullptr,
                      Opts.WantPairs ? &DeclPairs : nullptr))
        continue;
      StmtRanges.push_back(R);
      Out.push_back({DynTypedNode::create(*S), std::move(Bound), P,
                     std::move(Pairs), std::move(TypePairs),
                     std::move(DeclPairs)});
    }
    const auto *DS = dyn_cast<DeclStmt>(peel(Patterns[P]));
    if (!DS)
      continue;
    for (const Decl *D : FileScope) {
      if (ClaimedDecls.contains(D) ||
          overlaps(StmtRanges, D->getSourceRange()))
        continue;
      Bindings Bound = Seed;
      NodePairs Pairs;
      TypeLocPairs TypePairs;
      DeclarationPairs DeclPairs;
      if (!Shared.runOnDecl(*DS, D, Bound,
                            Opts.WantPairs ? &Pairs : nullptr,
                            Opts.WantPairs ? &TypePairs : nullptr,
                            Opts.WantPairs ? &DeclPairs : nullptr))
        continue;
      ClaimedDecls.insert(D);
      DeclRanges.push_back(D->getSourceRange());
      Out.push_back({DynTypedNode::create(*D), std::move(Bound), P,
                     std::move(Pairs), std::move(TypePairs),
                     std::move(DeclPairs)});
    }
  }

  // One pattern already comes out in source order, because the walk above is
  // outermost first. Several do not, since each is searched over the whole
  // translation unit before the next, and a caller reporting matches wants
  // them where the reader will look for them.
  if (Patterns.size() > 1)
    llvm::stable_sort(Out, [](const Match &A, const Match &B) {
      return A.Node.getSourceRange().getBegin() <
             B.Node.getSourceRange().getBegin();
    });
  return Out;
}

} // namespace clang::spatch

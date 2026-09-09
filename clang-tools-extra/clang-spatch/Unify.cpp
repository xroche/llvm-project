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
#include "llvm/ADT/SmallVector.h"

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

/// Do two written types, each read from its own translation unit, describe
/// the same type?
///
/// Written rather than canonical, because Coccinelle matches the spelling: a
/// pattern saying `long long` does not match a typedef that resolves to it.
/// A `QualType` cannot cross translation units, so nothing here compares one
/// by identity. A builtin compares by its kind, a named type by its name, and
/// everything else by structure.
///
/// **The class list here and the one in `uncomparableType` are one list.**
/// A class this does not handle must be refused there, or a pattern would
/// silently fail to match a construct it describes.
bool sameWrittenType(QualType P, QualType T) {
  if (P.getLocalFastQualifiers() != T.getLocalFastQualifiers())
    return false;
  const Type *PT = P.getTypePtrOrNull(), *TT = T.getTypePtrOrNull();
  if (!PT || !TT)
    return PT == TT;
  // A parenthesised declarator adds a layer that carries no meaning of its
  // own, and it is not always written on both sides.
  if (const auto *PP = dyn_cast<ParenType>(PT))
    return sameWrittenType(PP->getInnerType(), T);
  if (const auto *TP = dyn_cast<ParenType>(TT))
    return sameWrittenType(P, TP->getInnerType());
  if (PT->getTypeClass() != TT->getTypeClass())
    return false;
  if (const auto *PB = dyn_cast<BuiltinType>(PT))
    return PB->getKind() == cast<BuiltinType>(TT)->getKind();
  if (const auto *PPtr = dyn_cast<PointerType>(PT))
    return sameWrittenType(PPtr->getPointeeType(),
                           cast<PointerType>(TT)->getPointeeType());
  if (const auto *PC = dyn_cast<ComplexType>(PT))
    return sameWrittenType(PC->getElementType(),
                           cast<ComplexType>(TT)->getElementType());
  if (const auto *PA = dyn_cast<ConstantArrayType>(PT)) {
    const auto *TA = cast<ConstantArrayType>(TT);
    return PA->getSize() == TA->getSize() &&
           sameWrittenType(PA->getElementType(), TA->getElementType());
  }
  if (const auto *PA = dyn_cast<IncompleteArrayType>(PT))
    return sameWrittenType(PA->getElementType(),
                           cast<IncompleteArrayType>(TT)->getElementType());
  if (const auto *PD = dyn_cast<TypedefType>(PT))
    return PD->getDecl()->getName() ==
           cast<TypedefType>(TT)->getDecl()->getName();
  if (const auto *PR = dyn_cast<TagType>(PT)) {
    const TagDecl *PTag = PR->getDecl(), *TTag = cast<TagType>(TT)->getDecl();
    return PTag->getTagKind() == TTag->getTagKind() &&
           !PTag->getName().empty() && PTag->getName() == TTag->getName();
  }
  if (const auto *PF = dyn_cast<FunctionProtoType>(PT)) {
    const auto *TF = cast<FunctionProtoType>(TT);
    if (PF->isVariadic() != TF->isVariadic() ||
        PF->getNumParams() != TF->getNumParams() ||
        !sameWrittenType(PF->getReturnType(), TF->getReturnType()))
      return false;
    for (unsigned I = 0, E = PF->getNumParams(); I != E; ++I)
      if (!sameWrittenType(PF->getParamType(I), TF->getParamType(I)))
        return false;
    return true;
  }
  if (const auto *PF = dyn_cast<FunctionNoProtoType>(PT))
    return sameWrittenType(PF->getReturnType(),
                           cast<FunctionNoProtoType>(TT)->getReturnType());
  return false;
}

/// The type class in \p T that `sameWrittenType` cannot compare, or an empty
/// string. See the note on that function: the two class lists are one list.
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

class Unifier {
public:
  Unifier(const ParsedPattern &Parsed, ASTContext &Context)
      : Parsed(Parsed), Context(Context),
        PatternContext(Parsed.Unit->getASTContext()) {}

  bool run(const Stmt *Pattern, const Stmt *Target, Bindings &Bound) {
    return match(Pattern, Target, Bound);
  }

private:
  const ParsedPattern &Parsed;
  ASTContext &Context;        ///< The code's.
  ASTContext &PatternContext; ///< The pattern's, which is a separate TU.

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
      if (matchRun(P.Terms, Target, Start, Trial)) {
        Bound = std::move(Trial);
        return true;
      }
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

  /// Matches a declaration statement: the written type, the declared name and
  /// the initialiser, one declarator at a time.
  ///
  /// None of the three is a child of the `DeclStmt`, so comparing class plus
  /// children made every childless declaration match every other one.
  bool matchDecls(const DeclStmt &Pattern, const DeclStmt &Target,
                  Bindings &Bound) {
    auto PIt = Pattern.decl_begin(), PEnd = Pattern.decl_end();
    auto TIt = Target.decl_begin(), TEnd = Target.decl_end();
    for (; PIt != PEnd && TIt != TEnd; ++PIt, ++TIt) {
      const auto *PD = dyn_cast<DeclaratorDecl>(*PIt);
      const auto *TD = dyn_cast<DeclaratorDecl>(*TIt);
      const auto *PT = dyn_cast<TypedefNameDecl>(*PIt);
      const auto *TT = dyn_cast<TypedefNameDecl>(*TIt);
      if (PD && TD) {
        if (!matchWrittenType(PD->getTypeSourceInfo(), TD->getTypeSourceInfo(),
                              Bound) ||
            !matchDeclaredName(*PD, *TD, Bound))
          return false;
        const auto *PV = dyn_cast<VarDecl>(PD);
        const auto *TV = dyn_cast<VarDecl>(TD);
        if (!PV != !TV)
          return false;
        if (PV && !match(PV->getInit(), TV->getInit(), Bound))
          return false;
        continue;
      }
      if (!PT || !TT)
        return false;
      if (!matchWrittenType(PT->getTypeSourceInfo(), TT->getTypeSourceInfo(),
                            Bound) ||
          !matchDeclaredName(*PT, *TT, Bound))
        return false;
    }
    return PIt == PEnd && TIt == TEnd;
  }

  /// Matches a type written in the pattern against the one written in the
  /// target, binding a type metavariable to whatever the target wrote.
  bool matchWrittenType(const TypeSourceInfo *Pattern,
                        const TypeSourceInfo *Target, Bindings &Bound) {
    if (!Pattern || !Target)
      return false;
    if (const MetaVar *M = typeMetaVarOf(Pattern->getType()))
      return bindTo(*M, Binding{nullptr, Target->getTypeLoc().getSourceRange()},
                    Bound);
    return sameWrittenType(Pattern->getType(), Target->getType());
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

  /// The type metavariable \p T is, or null. `type T;` is synthesised as
  /// `typedef int T;`, so a pattern naming it carries that typedef.
  const MetaVar *typeMetaVarOf(QualType T) const {
    const auto *TD = dyn_cast_or_null<TypedefType>(T.getTypePtrOrNull());
    if (!TD)
      return nullptr;
    const MetaVar *M = Parsed.metaVarFor(TD->getDecl()->getCanonicalDecl());
    return M && M->Kind == MetaVar::Kind::Type ? M : nullptr;
  }

  bool match(const Stmt *Pattern, const Stmt *Target, Bindings &Bound) {
    if (!Pattern || !Target)
      return Pattern == Target;
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
      return matchDecls(*PD, *cast<DeclStmt>(T), Bound);
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
             FloatingLiteral, CharacterLiteral, StringLiteral>(&S);
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

std::string whyNotComparableInDecls(const DeclStmt &S) {
  for (const Decl *D : S.decls()) {
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
  }
  return std::string();
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

} // namespace

std::string whyNotComparable(const Stmt *Pattern) {
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
    if (std::string Why = whyNotComparable(Child); !Why.empty())
      return Why;
  return std::string();
}

bool unify(const Stmt *Pattern, const Stmt *Target, const ParsedPattern &Parsed,
           ASTContext &Context, Bindings &Bound) {
  return Unifier(Parsed, Context).run(Pattern, Target, Bound);
}

std::vector<Match> findMatches(const Stmt *Pattern, const ParsedPattern &Parsed,
                               ASTContext &Context) {
  std::vector<Match> Out;
  if (!Pattern)
    return Out;
  StmtCollector Collector;
  Collector.TraverseDecl(Context.getTranslationUnitDecl());
  Unifier Shared(Parsed, Context);

  // A match's subtrees are skipped, so one written call is reported once even
  // when the pattern would also match something inside it.
  llvm::SmallVector<const Stmt *, 8> Claimed;
  for (Stmt *S : Collector.All) {
    const bool Inside = llvm::any_of(Claimed, [&](const Stmt *C) {
      const SourceRange Outer = C->getSourceRange();
      const SourceRange Inner = S->getSourceRange();
      return Outer.getBegin() <= Inner.getBegin() &&
             Inner.getEnd() <= Outer.getEnd();
    });
    if (Inside)
      continue;
    Bindings Bound;
    if (!Shared.run(Pattern, S, Bound))
      continue;
    Claimed.push_back(S);
    Out.push_back({S, std::move(Bound)});
  }
  return Out;
}

} // namespace clang::spatch

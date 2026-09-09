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

/// Do two bound subtrees name the same thing?
///
/// A declaration reference is compared by declaration, so `l` in one function
/// is not the `l` of another. Anything else is compared by the text the author
/// wrote, which is what Coccinelle compares.
bool sameBinding(const Stmt *A, const Stmt *B, ASTContext &Context) {
  const Stmt *PA = peel(A), *PB = peel(B);
  const auto *RA = dyn_cast<DeclRefExpr>(PA);
  const auto *RB = dyn_cast<DeclRefExpr>(PB);
  if (RA && RB)
    return RA->getDecl()->getCanonicalDecl() ==
           RB->getDecl()->getCanonicalDecl();
  if (RA || RB)
    return false;
  return sourceTextOf(*PA, Context) == sourceTextOf(*PB, Context);
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
    auto It = Bound.find(M.Name);
    if (It != Bound.end())
      return sameBinding(It->second, Target, Context);
    Bound[M.Name] = Target;
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
  // `sizeof` over a type keeps the type off the child list, so `sizeof(int)`
  // and `sizeof(long)` are the same node plus the same no children.
  if (const auto *T = dyn_cast<UnaryExprOrTypeTraitExpr>(&S))
    return !T->isArgumentType();
  return isa<CompoundStmt, IfStmt, WhileStmt, DoStmt, ForStmt, ReturnStmt,
             BreakStmt, ContinueStmt, NullStmt, SwitchStmt, CaseStmt,
             DefaultStmt, ParenExpr, ArraySubscriptExpr, InitListExpr, StmtExpr,
             AbstractConditionalOperator>(&S);
}

/// The classes `Unifier::match` decides by more than class and children.
bool isComparedExplicitly(const Stmt &S) {
  return isa<BinaryOperator, UnaryOperator, DeclRefExpr, MemberExpr, CallExpr,
             IntegerLiteral, FloatingLiteral, CharacterLiteral, StringLiteral>(
      &S);
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

std::string uncomparableIn(const Stmt *Pattern) {
  if (!Pattern)
    return std::string();
  const Stmt *P = peel(Pattern);
  if (!P)
    return std::string();
  if (!isComparedExplicitly(*P) && !isComparableByStructure(*P))
    return P->getStmtClassName();
  for (const Stmt *Child : P->children())
    if (std::string Why = uncomparableIn(Child); !Why.empty())
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

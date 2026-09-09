//===--- PatchRunner.cpp - Apply a semantic patch to a TU ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PatchRunner.h"
#include "PathQuery.h"
#include "PatternCompiler.h"
#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

using namespace clang::ast_matchers;

namespace clang::spatch {

namespace {

/// The shape of rule body this version runs: an anchor statement, a
/// statement-level `...` carrying the `when !=` constraints, and optionally a
/// closing statement. Anything else is reported as unrun rather than
/// approximated.
struct RuleShape {
  const PatternItem *Anchor = nullptr;
  const PatternItem *Dots = nullptr;
};

std::optional<RuleShape> shapeOf(const Rule &R, std::string &Why) {
  RuleShape S;
  for (const PatternItem &I : R.Body) {
    if (I.Kind == PatternItem::Kind::Statement) {
      if (!S.Anchor) {
        S.Anchor = &I;
        continue;
      }
      if (!S.Dots) {
        Why = "two statements with no `...` between them needs a matcher over "
              "statement adjacency, which this version does not build";
        return std::nullopt;
      }
      continue; // A closing statement is accepted and not otherwise used.
    }
    if (S.Dots) {
      Why = "more than one `...` in a rule body needs ordering constraints "
            "between the anchors, which this version does not build";
      return std::nullopt;
    }
    S.Dots = &I;
  }
  if (!S.Anchor) {
    Why = "no statement to anchor the rule on";
    return std::nullopt;
  }
  if (!S.Dots) {
    Why = "no `...` in the rule body, so there is no path property to check";
    return std::nullopt;
  }
  return S;
}

/// Maps every statement in a CFG to its program point, so a matched node can
/// be located in the graph.
llvm::DenseMap<const Stmt *, Point> indexStatements(const CFGIndex &Index) {
  llvm::DenseMap<const Stmt *, Point> Map;
  for (CFGBlock *B : Index.graph()) {
    for (unsigned I = 0, E = B->size(); I != E; ++I) {
      const Point P{B->getBlockID(), I};
      if (const Stmt *S = Index.stmtAt(P))
        Map.try_emplace(S, P);
    }
  }
  return Map;
}

/// The declaration a pattern's shared metavariable is bound to, or null when
/// the binding is not a plain reference to one. Two constructs count as
/// naming the same resource when this returns the same declaration.
const ValueDecl *boundDecl(const BoundNodes &Nodes, llvm::StringRef Name) {
  if (const auto *DRE = Nodes.getNodeAs<DeclRefExpr>(Name))
    return DRE->getDecl();
  if (const auto *E = Nodes.getNodeAs<Expr>(Name))
    // IgnoreParenCasts rather than IgnoreParenImpCasts, so that an explicit
    // cast on one side of the pair still names the same resource. Treating
    // `unlock((lock_t *)l)` as a different resource from `lock(l)` reported a
    // release that was present as missing.
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenCasts()))
      return DRE->getDecl();
  return nullptr;
}

/// The function whose body contains \p S, found by walking parents.
///
/// A control-flow graph is built per function body, so an anchor is useless
/// until its function is known.
const FunctionDecl *enclosingFunction(const Stmt *S, ASTContext &Context) {
  llvm::SmallVector<DynTypedNode, 8> Work;
  for (const DynTypedNode &P : Context.getParents(*S))
    Work.push_back(P);
  while (!Work.empty()) {
    const DynTypedNode N = Work.pop_back_val();
    if (const auto *FD = N.get<FunctionDecl>())
      return FD->getBody() ? FD : nullptr;
    for (const DynTypedNode &P : Context.getParents(N))
      Work.push_back(P);
  }
  return nullptr;
}

} // namespace

void runPatch(const SemanticPatch &Patch, ASTContext &Context,
              RunResult &Result) {
  SourceManager &SM = Context.getSourceManager();

  for (const Rule &R : Patch.Rules) {
    std::string Why;
    std::optional<RuleShape> Shape = shapeOf(R, Why);
    if (!Shape) {
      Result.UnrunRules.push_back({R.Name, Why});
      continue;
    }
    if (!R.Quant) {
      Result.UnrunRules.push_back(
          {R.Name, "the rule has a `...` but states no `exists` or `forall`. "
                   "Coccinelle's default is a property of the whole file, "
                   "which does not port, so it must be stated"});
      continue;
    }

    std::string Error;
    std::optional<CompiledPattern> Anchor =
        compileCallPattern(Shape->Anchor->Text, R.MetaVars, Error);
    if (!Anchor) {
      Result.UnrunRules.push_back({R.Name, "anchor: " + Error});
      continue;
    }
    if (Shape->Dots->WhenNot.size() != 1) {
      Result.UnrunRules.push_back(
          {R.Name, "this version checks exactly one `when !=` constraint"});
      continue;
    }
    std::optional<CompiledPattern> Forbidden =
        compileCallPattern(Shape->Dots->WhenNot.front(), R.MetaVars, Error);
    if (!Forbidden) {
      Result.UnrunRules.push_back({R.Name, "when !=: " + Error});
      continue;
    }
    // The metavariable the two share is what ties the forbidden construct to
    // the anchor. Without one, `when != f(x)` would be satisfied by a release
    // of any resource, which is a different and much weaker property.
    std::string Shared;
    for (const std::string &B : Anchor->Bindings)
      if (llvm::is_contained(Forbidden->Bindings, B))
        Shared = B;
    if (Shared.empty()) {
      Result.UnrunRules.push_back(
          {R.Name, "the anchor and the `when !=` share no metavariable, so "
                   "the constraint would hold of any resource rather than "
                   "this one"});
      continue;
    }

    // Match every anchor in the translation unit, then group by the
    // function that contains it so each control-flow graph is built once.
    // Matching against a function body directly does not work, because the
    // match helpers test the node they are given rather than its descendants.
    llvm::DenseMap<const FunctionDecl *, std::unique_ptr<CFG>> Graphs;
    llvm::DenseMap<const FunctionDecl *, llvm::DenseMap<const Stmt *, Point>>
        Points;
    // `f(..., E, ...)` matches once per argument, and the rule holds of the
    // call when it holds of some position, so the first satisfying position
    // reports and the rest are dropped.
    llvm::DenseSet<const CallExpr *> Reported;

    for (const BoundNodes &Match : matchDynamic(Anchor->Matcher, Context)) {
      const auto *Call = Match.getNodeAs<CallExpr>("root");
      if (!Call || Reported.contains(Call))
        continue;
      const ValueDecl *Res = boundDecl(Match, Shared);
      if (!Res) {
        ++Result.AnchorsUnsupportedResource;
        continue;
      }
      const FunctionDecl *Fn = enclosingFunction(Call, Context);
      if (!Fn) {
        ++Result.AnchorsUnattributed;
        continue;
      }

      auto GraphIt = Graphs.find(Fn);
      if (GraphIt == Graphs.end()) {
        CFG::BuildOptions Opts;
        std::unique_ptr<CFG> G = CFG::buildCFG(
            Fn, const_cast<Stmt *>(Fn->getBody()), &Context, Opts);
        if (!G) {
          ++Result.FunctionsSkipped;
          Graphs[Fn] = nullptr;
          continue;
        }
        ++Result.FunctionsAnalysed;
        CFGIndex Idx(*G);
        Points[Fn] = indexStatements(Idx);
        GraphIt = Graphs.try_emplace(Fn, std::move(G)).first;
      }
      if (!GraphIt->second) {
        ++Result.AnchorsUnlocated;
        continue;
      }

      CFGIndex Index(*GraphIt->second);
      const auto &PointMap = Points[Fn];
      auto It = PointMap.find(static_cast<const Stmt *>(Call));
      if (It == PointMap.end()) {
        // The anchor matched but the graph gives it no program point, which
        // happens for a construct the CFG models differently.
        ++Result.AnchorsUnlocated;
        continue;
      }
      const Point After{It->second.Block, It->second.Elem + 1};

      const StmtPredicate IsForbidden = [&](const Stmt *S) {
        for (const BoundNodes &N :
             matchDynamic(Forbidden->Matcher, *S, Context))
          if (boundDecl(N, Shared) == Res)
            return true;
        return false;
      };

      const bool Report = *R.Quant == Rule::Quantifier::Exists
                              ? somePathAvoids(Index, After, IsForbidden)
                              : noPathReaches(Index, After, IsForbidden);
      if (!Report)
        continue;

      const PresumedLoc PL = SM.getPresumedLoc(Call->getBeginLoc());
      if (PL.isInvalid()) {
        // The property was decided and the report cannot be placed, which is a
        // lost finding rather than an absent one.
        ++Result.AnchorsUnattributed;
        continue;
      }
      Reported.insert(Call);
      Result.Findings.push_back(
          {PL.getFilename(), PL.getLine(), PL.getColumn(), R.Name,
           "`" + Forbidden->FunctionName + "` is absent on " +
               (*R.Quant == Rule::Quantifier::Exists ? "some" : "any") +
               " path from this `" + Anchor->FunctionName + "`"});
    }
  }
}

} // namespace clang::spatch

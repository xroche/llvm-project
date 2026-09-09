//===--- PatchRunner.cpp - Apply a semantic patch to a TU ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PatchRunner.h"
#include "Edit.h"
#include "PathQuery.h"
#include "Unify.h"
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

/// Does \p Pattern anywhere reference the metavariable named \p Name?
bool namesMetaVar(const Stmt *Pattern, llvm::StringRef Name,
                  const ParsedPattern &Parsed, ASTContext &Context) {
  if (!Pattern)
    return false;
  if (const auto *Ref = dyn_cast<DeclRefExpr>(Pattern->IgnoreContainers())) {
    if (const MetaVar *M =
            Parsed.metaVarFor(Ref->getDecl()->getCanonicalDecl()))
      if (M->Name == Name)
        return true;
  }
  for (const Stmt *Child : Pattern->children())
    if (namesMetaVar(Child, Name, Parsed, Context))
      return true;
  return false;
}

/// The declaration a binding names, or null when it is not a plain reference
/// to one. Two constructs count as naming the same resource when this returns
/// the same declaration.
const ValueDecl *boundDecl(const Bindings &Bound, llvm::StringRef Name) {
  auto It = Bound.find(Name);
  if (It == Bound.end())
    return nullptr;
  const auto *E = dyn_cast<Expr>(It->second);
  if (!E)
    return nullptr;
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

/// A rule body with no `...`, split into what it matches and what it writes.
///
/// A dot-free rule asks no question about control flow, so it needs no
/// quantifier and no path query. `runPatch` used to refuse every one of them
/// with "no `...` in the rule body, so there is no path property to check",
/// which is true and is not a reason to refuse: 339 of the 403 rules in the
/// sample corpus have no `...` at all.
struct FlatRule {
  const PatternItem *Match = nullptr; ///< The `-` or `*` line to match.
  std::string PlusText;               ///< The `+` lines, joined. May be empty.
  bool Rewrites = false; ///< Is this a `-`/`+` rule rather than `*`?
};

std::optional<FlatRule> flattenOf(const Rule &R, std::string &Why) {
  FlatRule F;
  unsigned Matches = 0, Contexts = 0;
  for (const PatternItem &I : R.Body) {
    if (I.Kind != PatternItem::Kind::Statement) {
      Why = "a dot-free rule holding a disjunction needs every branch matched "
            "at the same point, which this version does not build";
      return std::nullopt;
    }
    switch (I.Marker) {
    case PatternItem::Marker::Minus:
    case PatternItem::Marker::Star:
      F.Match = &I;
      F.Rewrites = I.Marker == PatternItem::Marker::Minus;
      ++Matches;
      break;
    case PatternItem::Marker::Plus:
      if (!F.PlusText.empty())
        F.PlusText += " ";
      F.PlusText += I.Text;
      break;
    case PatternItem::Marker::Context:
      ++Contexts;
      break;
    }
  }
  if (Matches != 1) {
    Why = Matches == 0
              ? "a dot-free rule with no `-` or `*` line has nothing to match"
              : "a dot-free rule matching more than one statement needs "
                "statement adjacency, which this version does not build";
    return std::nullopt;
  }
  if (Contexts != 0) {
    Why = "a dot-free rule with a context line needs the context matched "
          "beside the changed line, which this version does not build";
    return std::nullopt;
  }
  return F;
}

/// Runs a rule that asks no question about control flow.
void runFlatRule(const Rule &R, const FlatRule &F, ASTContext &Context,
                 RunResult &Result) {
  SourceManager &SM = Context.getSourceManager();
  std::string Error;
  // The pattern is parsed rather than compiled to a matcher expression, so
  // every statement form Clang can read is available and not only a call.
  std::optional<ParsedPattern> Parsed =
      parsePattern(R.MetaVars, {F.Match->Text}, Error);
  if (!Parsed) {
    Result.UnrunRules.push_back({R.Name, "pattern: " + Error});
    return;
  }
  if (!Parsed->Items[0]) {
    Result.UnrunRules.push_back({R.Name, "pattern: " + Parsed->Errors[0]});
    return;
  }

  for (const Match &M : findMatches(Parsed->Items[0], *Parsed, Context)) {
    const PresumedLoc PL = SM.getPresumedLoc(M.Node->getBeginLoc());
    if (PL.isInvalid()) {
      ++Result.AnchorsUnattributed;
      continue;
    }
    Result.Findings.push_back({PL.getFilename(), PL.getLine(), PL.getColumn(),
                               R.Name, "matches the pattern"});
    if (!F.Rewrites)
      continue;
    std::string EditError;
    std::optional<PatternEdit> E =
        buildEdit(*M.Node, F.PlusText, M.Bound, Context, EditError);
    if (!E) {
      ++Result.EditsRefused;
      continue;
    }
    if (llvm::Error Added =
            Result.Edits[E->Replacement.getFilePath()].add(E->Replacement)) {
      // Two matches asking for different text at one offset is a conflict
      // Replacements detects, and it must not be dropped quietly.
      llvm::consumeError(std::move(Added));
      ++Result.EditsRefused;
    }
  }
}

} // namespace

void runPatch(const SemanticPatch &Patch, ASTContext &Context,
              RunResult &Result) {
  SourceManager &SM = Context.getSourceManager();

  for (const Rule &R : Patch.Rules) {
    std::string Why;
    // A rule with no `...` asks nothing about control flow, so it takes the
    // flat path and needs no quantifier.
    const bool HasDots = llvm::any_of(R.Body, [](const PatternItem &I) {
      return I.Kind == PatternItem::Kind::Dots;
    });
    if (!HasDots) {
      if (std::optional<FlatRule> F = flattenOf(R, Why))
        runFlatRule(R, *F, Context, Result);
      else
        Result.UnrunRules.push_back({R.Name, Why});
      continue;
    }

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
    if (Shape->Dots->WhenNot.size() != 1) {
      Result.UnrunRules.push_back(
          {R.Name, "this version checks exactly one `when !=` constraint"});
      continue;
    }
    // Both statements are parsed together, so the anchor and the forbidden
    // construct share one translation unit and one set of metavariable
    // declarations.
    std::optional<ParsedPattern> Parsed = parsePattern(
        R.MetaVars, {Shape->Anchor->Text, Shape->Dots->WhenNot.front()}, Error);
    if (!Parsed) {
      Result.UnrunRules.push_back({R.Name, "pattern: " + Error});
      continue;
    }
    if (!Parsed->Items[0]) {
      Result.UnrunRules.push_back({R.Name, "anchor: " + Parsed->Errors[0]});
      continue;
    }
    if (!Parsed->Items[1]) {
      Result.UnrunRules.push_back({R.Name, "when !=: " + Parsed->Errors[1]});
      continue;
    }

    // The metavariable the two share is what ties the forbidden construct to
    // the anchor. Without one, `when != f(x)` would be satisfied by a release
    // of any resource, which is a different and much weaker property.
    std::string Shared;
    for (const MetaVar &M : R.MetaVars) {
      Bindings A, B;
      const bool InAnchor =
          namesMetaVar(Parsed->Items[0], M.Name, *Parsed, Context);
      const bool InWhen =
          namesMetaVar(Parsed->Items[1], M.Name, *Parsed, Context);
      if (InAnchor && InWhen) {
        Shared = M.Name;
        break;
      }
    }
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
    llvm::DenseSet<const Stmt *> Reported;

    for (const Match &M : findMatches(Parsed->Items[0], *Parsed, Context)) {
      const Stmt *Call = M.Node;
      if (Reported.contains(Call))
        continue;
      const ValueDecl *Res = boundDecl(M.Bound, Shared);
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
        Bindings Bound;
        if (!unify(Parsed->Items[1], S, *Parsed, Context, Bound))
          return false;
        return boundDecl(Bound, Shared) == Res;
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
           std::string("the `when !=` construct is absent on ") +
               (*R.Quant == Rule::Quantifier::Exists ? "some" : "any") +
               " path from this anchor"});
    }
  }
}

} // namespace clang::spatch

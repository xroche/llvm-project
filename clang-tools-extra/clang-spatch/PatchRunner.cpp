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
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include <cassert>

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
  for (const PatternItem &I : R.Minus) {
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
  const auto *E = dyn_cast_or_null<Expr>(It->second.Node);
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

/// What running a dot-free rule is for.
///
/// Outside `FlatRule` rather than in it, because the field that holds one is
/// also called `Purpose` and a nested enum of that name would be shadowed by
/// it wherever the type is spelled.
enum class RulePurpose {
  Rewrite, ///< A `-`/`+` rule: it reports its matches and edits them.
  Report,  ///< A `*` rule: it reports its matches and changes nothing.
  /// A rule that marks no line at all. It changes nothing and reports
  /// nothing, and it is run for the metavariable values a later rule
  /// inherits from it.
  Bind
};

/// A rule body with no `...`, reduced to the statements it matches and the
/// text that replaces each.
///
/// A dot-free rule asks no question about control flow, so it needs no
/// quantifier and no path query. `runPatch` used to refuse every one of them
/// with "no `...` in the rule body, so there is no path property to check",
/// which is true and is not a reason to refuse: 339 of the 403 rules in the
/// sample corpus have no `...` at all.
struct FlatRule {
  /// One statement to match and the text that replaces it. A rule body that
  /// is a disjunction has one per branch, in source order, and an earlier
  /// branch takes any text a later one also wants.
  struct Alternative {
    const PatternItem *Match = nullptr; ///< The statement to match.
    std::string PlusText; ///< What replaces it. Empty for a pure deletion.
    /// Was the `-` side written as a whole statement rather than as a bare
    /// expression? It decides whether the edit takes the terminator with it.
    bool PatternEndsInSemicolon = false;
    RulePurpose Purpose = RulePurpose::Rewrite;
  };

  std::vector<Alternative> Alts;
  RulePurpose Purpose = RulePurpose::Rewrite;
};

/// Do \p Items open with a `{` and close with a `}`?
///
/// Such a side is one construct written across several lines rather than a
/// sequence of statements, so its lines are an initialiser element or a record
/// member and not a statement each. `tests/defineinit.cocci`,
/// `tests/substruct.cocci` and `tests/td.cocci` are the corpus cases, and all
/// three used to be refused for missing statement adjacency, which is not what
/// any of them asks for.
bool opensABracedGroup(const std::vector<PatternItem> &Items) {
  const llvm::StringRef First = llvm::StringRef(Items.front().Text).trim();
  const llvm::StringRef Last = llvm::StringRef(Items.back().Text).trim();
  return First.ends_with("{") && (Last.ends_with("}") || Last.ends_with("};"));
}

/// Is \p Items a rule body that is nothing but one disjunction?
bool isOneDisjunction(const std::vector<PatternItem> &Items) {
  return Items.size() == 1 &&
         Items.front().Kind == PatternItem::Kind::Disjunction;
}

/// One alternative from one `-`/`+` side pair, which is either a whole rule
/// body or one branch of a disjunction.
std::optional<FlatRule::Alternative>
alternativeOf(const std::vector<PatternItem> &Minus,
              const std::vector<PatternItem> &Plus, std::string &Why) {
  for (const std::vector<PatternItem> *Side : {&Minus, &Plus})
    for (const PatternItem &I : *Side)
      if (I.Kind != PatternItem::Kind::Statement) {
        Why = I.Kind == PatternItem::Kind::Disjunction
                  ? "a disjunction that is not the whole rule body needs its "
                    "branches matched inside a larger pattern, which this "
                    "version does not build"
                  : "a `...` on a side of a dot-free rule body, which this "
                    "version does not build";
        return std::nullopt;
      }
  if (Minus.size() != 1) {
    if (Minus.empty())
      Why = "a dot-free rule with nothing to match on the `-` side";
    else if (opensABracedGroup(Minus))
      Why = "the `-` side is a brace-delimited group, so its lines are parts "
            "of one construct rather than a sequence of statements, and "
            "matching it needs a pattern for a node inside the braces";
    else
      Why = "a dot-free rule matching a sequence of statements needs "
            "statement adjacency, which this version does not build";
    return std::nullopt;
  }
  FlatRule::Alternative A;
  A.Match = &Minus.front();
  A.PatternEndsInSemicolon =
      llvm::StringRef(A.Match->Text).rtrim().ends_with(";");
  // A `*` rule reports and never rewrites, so its plus side is unread.
  if (A.Match->Marker == PatternItem::Marker::Star) {
    A.Purpose = RulePurpose::Report;
    return A;
  }
  if (A.Match->Marker != PatternItem::Marker::Minus) {
    // No `-` line is part of the statement, so the rule either changes
    // nothing or inserts beside it, and an insertion needs a position this
    // version does not decide.
    const bool Adds = llvm::any_of(Plus, [](const PatternItem &I) {
      return I.Marker == PatternItem::Marker::Plus;
    });
    if (Adds) {
      Why = "a dot-free rule that only inserts needs the insertion placed "
            "relative to the match, which this version does not build";
      return std::nullopt;
    }
    // The rule asks for no change and is still worth running, because a
    // later rule may declare `expression thisrule.X` and the values bound
    // here are the only ones that declaration admits.
    A.Purpose = RulePurpose::Bind;
    return A;
  }
  // One statement may be replaced by a sequence, so every plus-side statement
  // is part of the replacement text and none of them needs positioning. That
  // holds for an unmarked one too: `-if (e)` over `  kfree(e);` replaces the
  // `if` with its own body, and the body is what the plus side holds.
  //
  // A fragment is the case to refuse. `  if (a)` over `-   b();` leaves the
  // `if` head alone on the plus side, and writing that back would drop the
  // body and report success.
  for (const PatternItem &I : Plus) {
    if (I.Unfinished) {
      Why = "the rule takes part of a statement away and leaves a fragment, "
            "which needs an edit inside the matched node rather than over it";
      return std::nullopt;
    }
    if (!A.PlusText.empty())
      A.PlusText += " ";
    A.PlusText += I.Text;
  }
  A.Purpose = RulePurpose::Rewrite;
  return A;
}

std::optional<FlatRule> flattenOf(const Rule &R, std::string &Why) {
  FlatRule F;
  if (!isOneDisjunction(R.Minus) || !isOneDisjunction(R.Plus)) {
    std::optional<FlatRule::Alternative> A =
        alternativeOf(R.Minus, R.Plus, Why);
    if (!A)
      return std::nullopt;
    F.Purpose = A->Purpose;
    F.Alts.push_back(std::move(*A));
    return F;
  }

  // The branches are one search over several patterns rather than several
  // searches, because a branch may not take text an earlier branch already
  // took and that decision needs all of them at once. Grouping gives the two
  // sides the same branch count, since a branch is grouped once per side.
  const std::vector<std::vector<PatternItem>> &MinusBranches =
      R.Minus.front().Branches;
  const std::vector<std::vector<PatternItem>> &PlusBranches =
      R.Plus.front().Branches;
  // Grouping gives the two sides one entry per source branch, and a `(` in
  // column zero always closes with at least one branch, so the loop below can
  // index either side by the other's count.
  assert(!MinusBranches.empty() &&
         MinusBranches.size() == PlusBranches.size() &&
         "grouping produces one branch per side per source branch");
  for (unsigned I = 0, E = MinusBranches.size(); I != E; ++I) {
    std::optional<FlatRule::Alternative> A =
        alternativeOf(MinusBranches[I], PlusBranches[I], Why);
    if (!A) {
      Why = "branch " + std::to_string(I + 1) + " of the disjunction: " + Why;
      return std::nullopt;
    }
    // A rule that deletes in one branch and inserts in another asks for two
    // different edits from one match, and which one applies is decided per
    // site rather than per rule. Refusing says so instead of applying the
    // first branch's purpose to every site.
    if (!F.Alts.empty() && F.Alts.front().Purpose != A->Purpose) {
      Why = "the branches of the disjunction ask for different things, so the "
            "rule both rewrites and leaves alone depending on the branch, "
            "which this version does not build";
      return std::nullopt;
    }
    F.Alts.push_back(std::move(*A));
  }
  F.Purpose = F.Alts.front().Purpose;
  return F;
}

/// Runs a rule that asks no question about control flow, once per environment
/// in \p Seeds, and appends every environment its matches produced to \p Envs.
void runFlatRule(const Rule &R, const FlatRule &F,
                 llvm::ArrayRef<std::string> TypeNames,
                 llvm::ArrayRef<Bindings> Seeds, ASTContext &Context,
                 RunResult &Result, std::vector<Bindings> &Envs) {
  SourceManager &SM = Context.getSourceManager();
  std::string Error;
  // Every alternative is parsed in one call, so the branches of a disjunction
  // share one translation unit and one set of metavariable declarations, and
  // an alternative keeps the index its branch has.
  std::vector<std::string> Texts;
  for (const FlatRule::Alternative &A : F.Alts)
    Texts.push_back(A.Match->Text);
  // The pattern is parsed rather than compiled to a matcher expression, so
  // every statement form Clang can read is available and not only a call.
  std::optional<ParsedPattern> Parsed =
      parsePattern(R.MetaVars, Texts, TypeNames, Error);
  if (!Parsed) {
    Result.UnrunRules.push_back({R.Name, "pattern: " + Error});
    return;
  }

  // The patterns that can be read, in branch order, so that the first one
  // matching at a site is the earliest branch that matches there.
  std::vector<const Stmt *> Patterns(Texts.size(), nullptr);
  std::vector<Unrun> Unread;
  std::string FirstUnread;
  for (unsigned I = 0, E = Texts.size(); I != E; ++I) {
    std::string Why = Parsed->Items[I] ? whyNotComparable(Parsed->Items[I])
                                       : "pattern: " + Parsed->Errors[I];
    if (Why.empty()) {
      Patterns[I] = Parsed->Items[I];
      continue;
    }
    if (FirstUnread.empty())
      FirstUnread = Why;
    // A rule with one alternative and nothing to match is refused. One branch
    // of a disjunction is not: the other branches still describe sites this
    // patch changes, and Coccinelle applies them. Dropping it silently would
    // under-apply the patch and still report success, so the branch is
    // recorded and the run says it is incomplete.
    if (F.Alts.size() > 1)
      Unread.push_back(
          {R.Name, "branch " + std::to_string(I + 1) +
                       " of the disjunction "
                       "could not be read, so the sites it describes are "
                       "left alone: " +
                       Why});
  }
  // A rule with nothing left to run is refused rather than reported as a
  // partial run, so it is not recorded both ways.
  if (llvm::all_of(Patterns, [](const Stmt *P) { return !P; })) {
    Result.UnrunRules.push_back({R.Name, FirstUnread});
    return;
  }
  llvm::append_range(Result.UnreadBranches, std::move(Unread));

  const bool Rewrites = F.Purpose == RulePurpose::Rewrite;
  MatchOptions Opts;
  // A file-scope declaration of several things is withheld from a rewriting
  // rule because its declarators share one `;`. That is a reason not to edit
  // one, so a rule that builds no edit may still be shown it.
  Opts.MultiDeclaratorOK = !Rewrites;
  // One site can satisfy two environments, so it is taken by the first and
  // skipped by the rest. Two edits over one range would otherwise conflict.
  llvm::DenseSet<const void *> Taken;

  for (const Bindings &Seed : Seeds) {
    Opts.Inherited = &Seed;
    for (const Match &M : findMatches(Patterns, *Parsed, Context, Opts)) {
      const void *Id = M.Node.getMemoizationData();
      if (Id && !Taken.insert(Id).second)
        continue;
      const FlatRule::Alternative &A = F.Alts[M.Pattern];
      Envs.push_back(M.Bound);
      const PresumedLoc PL =
          SM.getPresumedLoc(M.Node.getSourceRange().getBegin());
      if (PL.isInvalid()) {
        ++Result.AnchorsUnattributed;
        continue;
      }
      // Coccinelle prints nothing for a rule that marks no line, and the two
      // tools' output is compared directly.
      if (F.Purpose != RulePurpose::Bind)
        Result.Findings.push_back({PL.getFilename(), PL.getLine(),
                                   PL.getColumn(), R.Name,
                                   "matches the pattern"});
      if (!Rewrites)
        continue;
      std::string EditError;
      std::optional<PatternEdit> E =
          buildEdit(M.Node, A.PlusText, A.PatternEndsInSemicolon, M.Bound,
                    Context, EditError);
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
}

/// The environments each named rule's matches produced. A rule that ran and
/// matched nothing has an entry holding none, and a rule that could not run
/// has no entry at all: the difference decides whether a rule inheriting from
/// it can be run.
using RuleEnvs = llvm::StringMap<std::vector<Bindings>>;

/// The environments \p R's inherited declarations admit, one entry each,
/// holding the inherited values alone.
///
/// A rule with no inherited declaration gets a single empty environment, so
/// that every caller runs the same loop. std::nullopt means the rule cannot be
/// run at all, and \p Why then says why.
std::optional<std::vector<Bindings>> inheritedSeeds(const Rule &R,
                                                    const RuleEnvs &Envs,
                                                    ASTContext &Context,
                                                    std::string &Why) {
  // The source rules in the order the declarations name them, so that the
  // product below is the same on every run.
  std::vector<std::string> Sources;
  for (const MetaVar &M : R.MetaVars)
    if (!M.InheritedFrom.empty() &&
        !llvm::is_contained(Sources, M.InheritedFrom))
      Sources.push_back(M.InheritedFrom);

  std::vector<Bindings> Out(1);
  for (const std::string &From : Sources) {
    auto It = Envs.find(From);
    if (It == Envs.end()) {
      Why = "the rule inherits a metavariable from rule '" + From +
            "', which did not run, so the values that metavariable may take "
            "are unknown and matching without them would rewrite more than "
            "the patch asks for";
      return std::nullopt;
    }
    std::vector<std::string> Names;
    for (const MetaVar &M : R.MetaVars)
      if (M.InheritedFrom == From)
        Names.push_back(M.Name);

    // Projected onto the names this rule takes, and deduplicated: the source
    // rule may have matched twenty times while binding the same two values.
    std::vector<Bindings> Projected;
    llvm::StringSet<> Seen;
    for (const Bindings &Env : It->second) {
      Bindings Take;
      std::string Key;
      bool Complete = true;
      for (const std::string &N : Names) {
        auto B = Env.find(N);
        if (B == Env.end()) {
          // The source rule matched without binding this name, which is what
          // a disjunction branch that does not mention it does. Coccinelle
          // leaves it unbound there, so the environment offers no value and
          // contributes nothing here.
          Complete = false;
          break;
        }
        Take[N] = B->second;
        Key += N + "=" + bindingKey(B->second, Context) + ";";
      }
      if (Complete && Seen.insert(Key).second)
        Projected.push_back(std::move(Take));
    }

    std::vector<Bindings> Combined;
    for (const Bindings &Have : Out)
      for (const Bindings &Add : Projected) {
        Bindings Merged = Have;
        for (const auto &Entry : Add)
          Merged[Entry.first()] = Entry.second;
        Combined.push_back(std::move(Merged));
      }
    Out = std::move(Combined);
  }
  return Out;
}

} // namespace

void runPatch(const SemanticPatch &Patch, ASTContext &Context,
              RunResult &Result) {
  SourceManager &SM = Context.getSourceManager();

  // Rules run in the order the patch writes them, which is what makes an
  // inherited declaration readable: every rule a later one can name has
  // already produced its environments.
  RuleEnvs Envs;
  // Recorded only for a rule that ran, so that an entry's absence says the
  // rule was refused rather than that it matched nothing.
  const auto record = [&](const Rule &R, std::vector<Bindings> Found) {
    if (!R.Name.empty())
      Envs[R.Name] = std::move(Found);
  };

  for (const Rule &R : Patch.Rules) {
    std::string Why;
    std::optional<std::vector<Bindings>> Seeds =
        inheritedSeeds(R, Envs, Context, Why);
    if (!Seeds) {
      Result.UnrunRules.push_back({R.Name, Why});
      continue;
    }
    std::vector<Bindings> Found;
    // A rule with no `...` asks nothing about control flow, so it takes the
    // flat path and needs no quantifier.
    const bool HasDots = llvm::any_of(R.Minus, [](const PatternItem &I) {
      return I.Kind == PatternItem::Kind::Dots;
    });
    if (!HasDots) {
      if (std::optional<FlatRule> F = flattenOf(R, Why)) {
        runFlatRule(R, *F, Patch.TypeNames, *Seeds, Context, Result, Found);
        record(R, std::move(Found));
      } else {
        Result.UnrunRules.push_back({R.Name, Why});
      }
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
        R.MetaVars, {Shape->Anchor->Text, Shape->Dots->WhenNot.front()},
        Patch.TypeNames, Error);
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
    if (std::string Why = whyNotComparable(Parsed->Items[0]); !Why.empty()) {
      Result.UnrunRules.push_back({R.Name, "anchor: " + Why});
      continue;
    }
    if (std::string Why = whyNotComparable(Parsed->Items[1]); !Why.empty()) {
      Result.UnrunRules.push_back({R.Name, "when !=: " + Why});
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

    // The names this rule inherits, so that the `when !=` below is read under
    // the same environment as the anchor rather than under a free one.
    std::vector<std::string> InheritedNames;
    for (const MetaVar &MV : R.MetaVars)
      if (!MV.InheritedFrom.empty())
        InheritedNames.push_back(MV.Name);

    // One anchor list over every inherited environment. A site two
    // environments both reach is kept once, so the walk below is the same as
    // it is for a rule that inherits nothing.
    std::vector<Match> Anchors;
    {
      llvm::DenseSet<const void *> Taken;
      MatchOptions Opts;
      for (const Bindings &Seed : *Seeds) {
        Opts.Inherited = &Seed;
        for (Match &Anchor :
             findMatches(Parsed->Items[0], *Parsed, Context, Opts)) {
          const void *Id = Anchor.Node.getMemoizationData();
          if (!Id || Taken.insert(Id).second)
            Anchors.push_back(std::move(Anchor));
        }
      }
    }

    for (const Match &M : Anchors) {
      // A path property is a property of a control-flow graph, and a
      // declaration outside every function body sits in none.
      const Stmt *Call = M.Node.get<Stmt>();
      if (!Call) {
        ++Result.AnchorsUnattributed;
        continue;
      }
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
        for (const std::string &N : InheritedNames)
          if (auto B = M.Bound.find(N); B != M.Bound.end())
            Bound[N] = B->second;
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
      Found.push_back(M.Bound);
      Result.Findings.push_back(
          {PL.getFilename(), PL.getLine(), PL.getColumn(), R.Name,
           std::string("the `when !=` construct is absent on ") +
               (*R.Quant == Rule::Quantifier::Exists ? "some" : "any") +
               " path from this anchor"});
    }
    record(R, std::move(Found));
  }

  // Widening happens here rather than in `buildEdit`, because the rule needs
  // every deletion of a file at once: two deletions separated by a blank line
  // are one region to Coccinelle, and widening them one at a time keeps the
  // blank line that separated them.
  for (auto &File : Result.Edits) {
    auto Entry = SM.getFileManager().getOptionalFileRef(File.first());
    if (!Entry)
      continue;
    const FileID FID = SM.translateFile(*Entry);
    if (FID.isInvalid())
      continue;
    bool Invalid = false;
    const llvm::StringRef Buffer = SM.getBufferData(FID, &Invalid);
    if (Invalid)
      continue;
    File.second = widenDeletions(File.first(), Buffer, File.second);
  }
}

} // namespace clang::spatch

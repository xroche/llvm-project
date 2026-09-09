//===--- PatternCompilerTest.cpp - Tests for the pattern compiler --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PatchRunner.h"
#include "PatternParser.h"
#include "SmplParser.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"

namespace clang::spatch {
namespace {

/// How many times the compiled pattern matches in \p Code, which for the

} // namespace

TEST(ArgumentDotsShape, EachShapeIsToldApart) {
  EXPECT_EQ(ArgDotsShape::Bare, argumentDotsShape("..."));
  EXPECT_EQ(ArgDotsShape::Prefix, argumentDotsShape("E, ..."));
  EXPECT_EQ(ArgDotsShape::Prefix, argumentDotsShape("E1, E2, ..."));
  EXPECT_EQ(ArgDotsShape::Suffix, argumentDotsShape("..., E"));
  EXPECT_EQ(ArgDotsShape::Surrounded, argumentDotsShape("..., E, ..."));
  EXPECT_EQ(ArgDotsShape::Surrounded, argumentDotsShape("..., X, Y, ..."));
  // Dots with a named term on each side need a position counted from each
  // end, which the unifier does not do, so this one stays refused.
  EXPECT_EQ(ArgDotsShape::Interior, argumentDotsShape("E1, ..., E2"));
  // A comma inside a nested call does not split the list.
  EXPECT_EQ(ArgDotsShape::Prefix, argumentDotsShape("g(a, b), ..."));
  EXPECT_EQ(ArgDotsShape::NotDotted, argumentDotsShape("E1, E2"));
}

TEST(RunPatch, ASurroundedAnchorReportsItsCallOnceAndCanStaySilent) {
  const llvm::StringRef Patch = "@r exists@\nexpression E;\n@@\n"
                                "lock(..., E, ...);\n"
                                "... when != unlock(E)\n";
  std::string Error;
  std::optional<SemanticPatch> P = parseSemanticPatch(Patch, "t.cocci", Error);
  ASSERT_TRUE(P.has_value()) << Error;
  ASSERT_TRUE(P->fullyUnderstood());

  const llvm::StringRef Decls = "void lock(int*,int*,int*);\n"
                                "void unlock(int*);\n";
  auto findings = [&](llvm::StringRef Body) {
    std::unique_ptr<ASTUnit> Unit = tooling::buildASTFromCodeWithArgs(
        (Decls + "void f(int *a,int *b,int *c){ lock(a,b,c); " + Body + " }\n")
            .str(),
        {"-std=c11", "-w"});
    RunResult R;
    runPatch(*P, Unit->getASTContext(), R);
    return R.Findings.size();
  };

  // Three arguments match the anchor, and the call is one site, so it is
  // reported once rather than once per argument.
  EXPECT_EQ(1u, findings(""));
  EXPECT_EQ(1u, findings("unlock(b);"));
  // Releasing every argument leaves nothing to report, which is what proves
  // the report is not simply always emitted.
  EXPECT_EQ(0u, findings("unlock(a); unlock(b); unlock(c);"));
}

TEST(RunPatch, BothQuantifiersAgreeWithCoccinelleOnTheSameFour) {
  // The expected line sets are Coccinelle 1.3.3's own verdicts on this file,
  // taken by running the equivalent patch through spatch and reading the
  // lines it rewrote. Recorded here so the comparison is a regression test
  // rather than a measurement somebody has to repeat by hand.
  //
  // `unlockedOnOneBranch` separates the quantifiers, and
  // `unlockedBehindAGoto` pins that a construct the function cannot reach
  // counts as absent, which is what makes the answer control-flow sensitive
  // rather than syntactic.
  // Only the four anchor lines are annotated, because those are the numbers
  // the assertions name.
  const llvm::StringRef Code = "void mutex_lock(int *l);\n"
                               "void mutex_unlock(int *l);\n"
                               "void other(void);\n"
                               "void unlockedOnBothBranches"
                               "(int *l, int x) {\n"
                               "  mutex_lock(l);\n" // line 5
                               "  if (x) { mutex_unlock(l); }\n"
                               "  else { mutex_unlock(l); }\n"
                               "}\n"
                               "void unlockedOnOneBranch"
                               "(int *l, int x) {\n"
                               "  mutex_lock(l);\n" // line 10
                               "  if (x) { mutex_unlock(l); }\n"
                               "  other();\n"
                               "}\n"
                               "void neverUnlocked(int *l) {\n"
                               "  mutex_lock(l);\n" // line 15
                               "  other();\n"
                               "}\n"
                               "void unlockedBehindAGoto"
                               "(int *l) {\n"
                               "  mutex_lock(l);\n" // line 19
                               "  goto out;\n"
                               "  mutex_unlock(l);\n"
                               "out:\n"
                               "  other();\n"
                               "}\n";

  auto reportedLines = [&](llvm::StringRef Quantifier) {
    const std::string Patch = ("@r " + Quantifier +
                               "@\nexpression l;\n@@\n\n* mutex_lock(l);\n"
                               "  ... when != mutex_unlock(l)\n")
                                  .str();
    std::string Error;
    std::optional<SemanticPatch> P =
        parseSemanticPatch(Patch, "t.cocci", Error);
    std::vector<unsigned> Lines;
    if (!P || !P->fullyUnderstood())
      return Lines;
    std::unique_ptr<ASTUnit> Unit =
        tooling::buildASTFromCodeWithArgs(Code, {"-std=c11", "-w"});
    RunResult R;
    runPatch(*P, Unit->getASTContext(), R);
    for (const Finding &F : R.Findings)
      Lines.push_back(F.Line);
    llvm::sort(Lines);
    return Lines;
  };

  // The lock unlocked on one branch only is reported under `exists` and not
  // under `forall`, which is the whole difference between them.
  EXPECT_EQ(std::vector<unsigned>({10u, 15u, 19u}), reportedLines("exists"));
  EXPECT_EQ(std::vector<unsigned>({15u, 19u}), reportedLines("forall"));
}

TEST(RunPatch, ALockHeldAcrossANonTerminatingLoopIsReported) {
  // Coccinelle 1.3.3's verdicts on this file, taken from the oracle. A loop
  // gets a synthetic fall-through node there even when its condition can
  // never be false, so its exit is reachable and the lock at line 5 is a
  // leak. Clang nulls the same edge instead of dropping it, which is why the
  // walk treats a null successor with no block behind it as the exit.
  const llvm::StringRef Code = "void mutex_lock(int *l);\n"    // 1
                               "void mutex_unlock(int *l);\n"  // 2
                               "void other(void);\n"           // 3
                               "void heldAcrossAnInfiniteLoop" // 4
                               "(int *l) {\n"
                               "  mutex_lock(l);\n"         // 5
                               "  for (;;) { other(); }\n"  // 6
                               "}\n"                        // 7
                               "void releasedInsideTheLoop" // 8
                               "(int *l) {\n"
                               "  mutex_lock(l);\n"                // 9
                               "  for (;;) { mutex_unlock(l); }\n" // 10
                               "}\n"                               // 11
                               "void releasedAfterAFiniteLoop"     // 12
                               "(int *l, int n) {\n"
                               "  mutex_lock(l);\n"           // 13
                               "  while (n--) { other(); }\n" // 14
                               "  mutex_unlock(l);\n"         // 15
                               "}\n";                         // 16

  auto reportedLines = [&](llvm::StringRef Quantifier) {
    const std::string Patch = ("@r " + Quantifier +
                               "@\nexpression l;\n@@\n\n* mutex_lock(l);\n"
                               "  ... when != mutex_unlock(l)\n")
                                  .str();
    std::string Error;
    std::optional<SemanticPatch> P =
        parseSemanticPatch(Patch, "t.cocci", Error);
    std::vector<unsigned> Lines;
    if (!P || !P->fullyUnderstood())
      return Lines;
    std::unique_ptr<ASTUnit> Unit =
        tooling::buildASTFromCodeWithArgs(Code, {"-std=c11", "-w"});
    RunResult R;
    runPatch(*P, Unit->getASTContext(), R);
    for (const Finding &F : R.Findings)
      Lines.push_back(F.Line);
    llvm::sort(Lines);
    return Lines;
  };

  // Measured against spatch 1.3.3 on this exact source rather than reasoned
  // about. The lock at 9 is reported under `exists` because the synthetic
  // fall-through bypasses the loop body holding the release, and not under
  // `forall` because the path through the body does release it. The lock at 13
  // is released on the path that leaves the loop, so neither quantifier
  // reports it.
  EXPECT_EQ(std::vector<unsigned>({5u, 9u}), reportedLines("exists"));
  EXPECT_EQ(std::vector<unsigned>({5u}), reportedLines("forall"));
}

TEST(RunPatch, DeadCodeAfterANonTerminatingLoopDivergesFromCoccinelle) {
  // A known gap, pinned so it is not mistaken for a regression. Coccinelle
  // walks its synthetic fall-through into whatever follows the loop even when
  // no execution reaches it, so it sees the release and stays quiet. Clang
  // keeps the unreachable block in the function's block list without wiring
  // the nulled edge to it, so the release is invisible here and the lock is
  // reported. Wiring it would mean guessing which orphan block the edge meant.
  const llvm::StringRef Code = "void mutex_lock(int *l);\n"
                               "void mutex_unlock(int *l);\n"
                               "void other(void);\n"
                               "void releasedByDeadCode(int *l) {\n"
                               "  mutex_lock(l);\n" // line 5
                               "  for (;;) { other(); }\n"
                               "  mutex_unlock(l);\n"
                               "}\n";
  std::string Error;
  std::optional<SemanticPatch> P =
      parseSemanticPatch("@r exists@\nexpression l;\n@@\n\n* mutex_lock(l);\n"
                         "  ... when != mutex_unlock(l)\n",
                         "t.cocci", Error);
  ASSERT_TRUE(P.has_value()) << Error;
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c11", "-w"});
  RunResult R;
  runPatch(*P, Unit->getASTContext(), R);
  ASSERT_EQ(1u, R.Findings.size());
  EXPECT_EQ(5u, R.Findings[0].Line) << "Coccinelle reports nothing here";
}

} // namespace clang::spatch

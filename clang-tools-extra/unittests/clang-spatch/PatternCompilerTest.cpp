//===--- PatternCompilerTest.cpp - Tests for the pattern compiler --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PatternCompiler.h"
#include "PatchRunner.h"
#include "SmplParser.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"

using namespace clang::ast_matchers;

namespace clang::spatch {
namespace {

std::vector<MetaVar> expressions(std::initializer_list<const char *> Names) {
  std::vector<MetaVar> Out;
  for (const char *N : Names)
    Out.push_back(MetaVar{MetaVar::Kind::Expression, N, {}});
  return Out;
}

/// The matcher source \p Text compiles to, or the refusal prefixed by "!".
std::string compiled(llvm::StringRef Text, llvm::ArrayRef<MetaVar> MetaVars) {
  std::string Error;
  std::optional<CompiledPattern> P = compileCallPattern(Text, MetaVars, Error);
  return P ? P->MatcherSource : "!" + Error;
}

/// How many times the compiled pattern matches in \p Code, which for the
/// surrounded shape is once per candidate argument.
unsigned matchCount(llvm::StringRef Text, llvm::ArrayRef<MetaVar> MetaVars,
                    llvm::StringRef Code,
                    std::vector<std::string> Args = {"-std=c11", "-w"}) {
  std::string Error;
  std::optional<CompiledPattern> P = compileCallPattern(Text, MetaVars, Error);
  if (!P)
    return 0;
  std::unique_ptr<ASTUnit> Unit = tooling::buildASTFromCodeWithArgs(Code, Args);
  if (!Unit)
    return 0;
  return matchDynamic(P->Matcher, Unit->getASTContext()).size();
}

} // namespace

TEST(ArgumentDotsShape, EachShapeIsToldApart) {
  EXPECT_EQ(ArgDotsShape::Bare, argumentDotsShape("..."));
  EXPECT_EQ(ArgDotsShape::Prefix, argumentDotsShape("E, ..."));
  EXPECT_EQ(ArgDotsShape::Prefix, argumentDotsShape("E1, E2, ..."));
  EXPECT_EQ(ArgDotsShape::Surrounded, argumentDotsShape("..., E, ..."));
  // A named argument after the dots needs its position, and the position is
  // what the dots leave undetermined.
  EXPECT_EQ(ArgDotsShape::Other, argumentDotsShape("..., E"));
  EXPECT_EQ(ArgDotsShape::Other, argumentDotsShape("E1, ..., E2"));
  // Two named arguments between the dots have to be adjacent and in order,
  // and two enumerations give the cross product instead.
  EXPECT_EQ(ArgDotsShape::Other, argumentDotsShape("..., X, Y, ..."));
  // A comma inside a nested call does not split the list.
  EXPECT_EQ(ArgDotsShape::Prefix, argumentDotsShape("g(a, b), ..."));
  EXPECT_EQ(ArgDotsShape::Other, argumentDotsShape("E1, E2"));
}

TEST(CompileCallPattern, BareDotsDropTheCountAndConstrainNothing) {
  EXPECT_EQ("callExpr(callee(functionDecl(hasName(\"f\")))).bind(\"root\")",
            compiled("f(...);", {}));
  // Without the dots the count is pinned, so a two-argument pattern cannot
  // match a three-argument call.
  EXPECT_EQ("callExpr(callee(functionDecl(hasName(\"f\"))), "
            "argumentCountIs(0)).bind(\"root\")",
            compiled("f();", {}));
}

TEST(CompileCallPattern, PrefixDotsLeaveTheNamedArgumentsPositional) {
  const std::vector<MetaVar> MV = expressions({"E1", "E2"});
  EXPECT_EQ("callExpr(callee(functionDecl(hasName(\"f\"))), "
            "hasArgument(0, expr().bind(\"E1\")), "
            "hasArgument(1, expr().bind(\"E2\"))).bind(\"root\")",
            compiled("f(E1, E2, ...);", MV));
}

TEST(CompileCallPattern, SurroundedDotsEnumerateAndExcludeWhatIsNotAnArgument) {
  const std::vector<MetaVar> MV = expressions({"E"});
  // The callee binding must precede the enumeration: equalsBoundNode on an id
  // that is not yet bound lets every node through, so a swapped order would
  // bind the callee as an argument and silently over-match.
  EXPECT_EQ("callExpr(callee(functionDecl(hasName(\"f\"))), "
            "callee(expr().bind(\"spatch.callee\")), "
            "forEach(expr(unless(equalsBoundNode(\"spatch.callee\")), "
            "unless(cxxDefaultArgExpr())).bind(\"E\"))).bind(\"root\")",
            compiled("f(..., E, ...);", MV));
}

TEST(CompileCallPattern, ShapesOutsideTheSubsetAreRefused) {
  const std::vector<MetaVar> MV = expressions({"E", "X", "Y"});
  EXPECT_EQ("!the argument list puts a named argument after a `...`, which "
            "needs the argument's position and it is not determined",
            compiled("f(..., E);", MV));
  EXPECT_EQ("!the argument list puts a named argument after a `...`, which "
            "needs the argument's position and it is not determined",
            compiled("f(..., X, Y, ...);", MV));
}

TEST(CompileCallPattern, AMetavariableCalleeIsRefusedRatherThanNameMatched) {
  const std::vector<MetaVar> MV = expressions({"f"});
  // Emitting hasName("f") here matches a function that happens to be called
  // f and misses every call the rule means, while reporting success.
  EXPECT_EQ("!callee `f` is a metavariable, and matching a callee by a "
            "metavariable needs a binding over the callee rather than a name",
            compiled("f(1);", MV));
  EXPECT_EQ("!callee `f` is a metavariable, and matching a callee by a "
            "metavariable needs a binding over the callee rather than a name",
            compiled("f(...);", MV));
}

TEST(SurroundedShape, BindsEveryWrittenArgumentAndNothingElse) {
  const std::vector<MetaVar> MV = expressions({"E"});
  const llvm::StringRef Pattern = "f(..., E, ...);";

  // A declared parameter list, so every argument has a ParmVarDecl.
  EXPECT_EQ(3u, matchCount(Pattern, MV,
                           "void f(int,int,int);\n"
                           "void c(int a,int b,int d){ f(a,b,d); }\n"));
  // A variadic callee. forEachArgumentWithParam sees only the declared
  // parameter here and would report 1, which is a silent under-match.
  EXPECT_EQ(3u, matchCount(Pattern, MV,
                           "void f(int, ...);\n"
                           "void c(int a,int b,int d){ f(a,b,d); }\n"));
  // An argument that is itself a call is one argument, not a subtree to
  // descend into.
  EXPECT_EQ(2u, matchCount(Pattern, MV,
                           "int g(int);\nvoid f(int, ...);\n"
                           "void c(int a,int b){ f(g(a),b); }\n"));
  // An argument that names a function decays through the same cast as the
  // callee, so excluding the callee by cast kind would drop it.
  EXPECT_EQ(2u, matchCount(Pattern, MV,
                           "int h(int);\nvoid f(int, ...);\n"
                           "void c(int a){ f(a, h); }\n"));
  // A C++ default argument is not a written argument, and Coccinelle does not
  // match one.
  EXPECT_EQ(1u, matchCount(Pattern, MV,
                           "void f(int, int = 3);\n"
                           "void c(int a){ f(a); }\n",
                           {"-std=c++17", "-w"}));
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

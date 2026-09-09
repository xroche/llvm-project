//===--- EditTest.cpp - Tests for the dot-free path and rewriting --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Edit.h"
#include "PatchRunner.h"
#include "SmplParser.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"

namespace clang::spatch {
namespace {

/// Runs \p Patch over \p Code and returns the rewritten source, or "!" plus
/// the first reason no rule ran.
std::string rewritten(llvm::StringRef Patch, llvm::StringRef Code) {
  std::string Error;
  std::optional<SemanticPatch> P = parseSemanticPatch(Patch, "t.cocci", Error);
  if (!P)
    return "!" + Error;
  if (!P->fullyUnderstood()) {
    std::string Refusals;
    for (const Refusal &R : P->Refusals)
      Refusals += R.Construct + "; ";
    return "!refused: " + Refusals;
  }
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=gnu11", "-w"}, "input.c");
  if (!Unit)
    return "!no AST";
  RunResult Result;
  runPatch(*P, Unit->getASTContext(), Result);
  if (!Result.UnrunRules.empty())
    return "!" + Result.UnrunRules.front().Reason;
  if (Result.Edits.empty())
    return Code.str();
  llvm::Expected<std::string> Out =
      tooling::applyAllReplacements(Code, Result.Edits.begin()->second);
  if (!Out)
    return "!" + llvm::toString(Out.takeError());
  return *Out;
}

} // namespace

TEST(FlatRule, ADotFreeRuleRunsAndRewrites) {
  // Before this, `runPatch` refused every rule with no `...`, saying there was
  // no path property to check. That is true and is not a reason to refuse:
  // 339 of the 403 rules in the sample corpus have no `...` at all.
  EXPECT_EQ("void foo(int);\nvoid bar(int);\nvoid f(int x) { bar(x + 1); }\n",
            rewritten("@r@\nexpression E;\n@@\n- foo(E);\n+ bar(E);\n",
                      "void foo(int);\nvoid bar(int);\n"
                      "void f(int x) { foo(x + 1); }\n"));
}

TEST(FlatRule, AMetavariableCarriesTheTextItMatched) {
  // The replacement takes the argument's source text, so an expression with
  // its own spacing and operators survives verbatim.
  EXPECT_EQ("void g(int);\nvoid h(int);\nvoid f(int a, int b) "
            "{ h(a * 2 + b); }\n",
            rewritten("@r@\nexpression E;\n@@\n- g(E);\n+ h(E);\n",
                      "void g(int);\nvoid h(int);\nvoid f(int a, int b) "
                      "{ g(a * 2 + b); }\n"));
}

TEST(FlatRule, EveryMatchIsRewrittenRatherThanTheFirst) {
  EXPECT_EQ("void foo(int);\nvoid bar(int);\n"
            "void f(void) { bar(1); bar(2); }\n",
            rewritten("@r@\nconstant C;\n@@\n- foo(C);\n+ bar(C);\n",
                      "void foo(int);\nvoid bar(int);\n"
                      "void f(void) { foo(1); foo(2); }\n"));
}

TEST(FlatRule, AMinusWithNoPlusDeletesTheStatement) {
  EXPECT_EQ("void foo(int);\nvoid f(int x) {  }\n",
            rewritten("@r@\nexpression E;\n@@\n- foo(E);\n",
                      "void foo(int);\nvoid f(int x) { foo(x); }\n"));
}

TEST(FlatRule, AStarRuleMatchesAndChangesNothing) {
  const llvm::StringRef Code = "void foo(int);\nvoid f(int x) { foo(x); }\n";
  EXPECT_EQ(Code.str(), rewritten("@r@\nexpression E;\n@@\n* foo(E);\n", Code));
}

TEST(FlatRule, ASubExpressionMatchKeepsTheStatementTerminator) {
  // The semicolon belongs to the `return`, not to the operand replaced.
  // Extending the edit over it unconditionally produced `return 10` and the
  // file no longer compiled, which is what `tests/hashhash.cocci`,
  // `tests/hil1.cocci` and `tests/sizeof.cocci` disagreed with Coccinelle on.
  EXPECT_EQ("int f(void) { return 10; }\n",
            rewritten("@r@\n@@\n- 12\n+ 10\n", "int f(void) { return 12; }\n"));
}

TEST(FlatRule, ShapesOutsideTheFlatPathAreNamedRatherThanRun) {
  // Each of these is a real Coccinelle shape and none is silently
  // approximated, because a rule that half-runs leaves code matching neither
  // the old pattern nor the new one.
  EXPECT_EQ("!a dot-free rule matching more than one statement needs "
            "statement adjacency, which this version does not build",
            rewritten("@r@\n@@\n- foo();\n- bar();\n",
                      "void foo(void);\nvoid bar(void);\nvoid f(void) "
                      "{ foo(); bar(); }\n"));
  EXPECT_EQ("!a dot-free rule with a context line needs the context matched "
            "beside the changed line, which this version does not build",
            rewritten("@r@\n@@\n  foo();\n- bar();\n",
                      "void foo(void);\nvoid bar(void);\nvoid f(void) "
                      "{ foo(); bar(); }\n"));
  // A `+`-only rule never reaches the runner, because the parser rejects it
  // first with Coccinelle's own wording. The runner's guard for it stays as a
  // guard rather than a reachable path.
  EXPECT_EQ("!t.cocci:1: a '+' slice with no '-' line and no context line: "
            "Coccinelle reports \"minus slice can't be empty\"",
            rewritten("@r@\n@@\n+ foo();\n", "void f(void) { }\n"));
}

TEST(SourceTextOf, AMacroArgumentComesThroughAsWritten) {
  // The replacement must carry what the author wrote, not what the
  // preprocessor produced, so a matched argument spelled as a macro keeps its
  // spelling.
  EXPECT_EQ("void foo(int);\nvoid bar(int);\n#define N 40 + 2\n"
            "void f(void) { bar(N); }\n",
            rewritten("@r@\nexpression E;\n@@\n- foo(E);\n+ bar(E);\n",
                      "void foo(int);\nvoid bar(int);\n#define N 40 + 2\n"
                      "void f(void) { foo(N); }\n"));
}

} // namespace clang::spatch

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

TEST(FlatRule, AContextLineInsideTheChangedStatementIsMatchedWithIt) {
  // The head of a transformed `if` sits on a context line and its condition
  // on a `-` and a `+` line, so the two sides of the rule are
  // `if (E) foo(E);` and `if (!E) foo(E);`. Grouping by marker instead left
  // the head and the body as separate fragments and neither parsed.
  EXPECT_EQ("void foo(int);\nvoid f(int x) { if (!x) foo(x); }\n",
            rewritten("@r@\nexpression E;\n@@\n- if (E)\n+ if (!E)\n"
                      "    foo(E);\n",
                      "void foo(int);\nvoid f(int x) { if (x) foo(x); }\n"));
}

TEST(FlatRule, ALineEndingInAnOperatorTakesTheContextLineBelowIt) {
  // `tests/unary.cocci` is `- -` over ` x`, so the minus side is the unary
  // expression `- x` and the plus side is `x` alone. Neither line is a
  // pattern on its own.
  EXPECT_EQ("int f(void) { return 1; }\n",
            rewritten("@r@\nexpression x;\n@@\n- -\n x\n",
                      "int f(void) { return -1; }\n"));
}

TEST(FlatRule, OneStatementIsReplacedByASequenceOfThem) {
  // `tests/test7.cocci` is this shape. The plus side is two statements and
  // both replace the one that matched, so neither needs positioning.
  EXPECT_EQ("void foo(int);\nvoid f(void) { foo(1); foo(2); }\n",
            rewritten("@r@\nconstant C;\n@@\n- foo(C);\n+ foo(1);\n"
                      "+ foo(2);\n",
                      "void foo(int);\nvoid f(void) { foo(9); }\n"));
}

TEST(FlatRule, DroppingAStatementHeadKeepsWhatIsLeftOfIt) {
  // `tests/unfree.cocci` is this shape: the `if` goes and its body stays. The
  // body is the whole of the plus side, so replacing the matched `if` with it
  // is the edit the rule asks for.
  EXPECT_EQ("void b(void);\nvoid f(int a) { b(); }\n",
            rewritten("@r@\n@@\n- if (a)\n    b();\n",
                      "void b(void);\nvoid f(int a) { if (a) b(); }\n"));
}

TEST(FlatRule, LeavingAFragmentOnThePlusSideIsRefused) {
  // Here the body goes and the `if` head is left alone on the plus side.
  // Writing that back would drop the body and report success.
  EXPECT_EQ("!the rule takes part of a statement away and leaves a fragment, "
            "which needs an edit inside the matched node rather than over it",
            rewritten("@r@\n@@\n  if (a)\n-   b();\n",
                      "void b(void);\nvoid f(int a) { if (a) b(); }\n"));
}

TEST(FlatRule, ADeclarationComparesItsTypeAndCarriesItsName) {
  // A `DeclStmt` with no initialiser has no children, and its type and its
  // declared name are not children either. Compared by class plus children,
  // this pattern matched `char c;` and `struct S { int f; } s;` as well, and
  // rewrote all three to the literal text `int x;`.
  EXPECT_EQ("int f(void) { int b; char c; return 0; }\n",
            rewritten("@r@\nidentifier x;\n@@\n- long long x;\n+ int x;\n",
                      "int f(void) { long long b; char c; return 0; }\n"));
}

TEST(FlatRule, ATypeMetavariableCarriesTheTypeItMatched) {
  // `T` stands for whatever the target declared, so both declarations match
  // and each keeps its own type.
  EXPECT_EQ("int f(void) { long b = 0; char c = 0; return 0; }\n",
            rewritten("@r@\ntype T;\nidentifier x;\n@@\n- T x;\n"
                      "+ T x = 0;\n",
                      "int f(void) { long b; char c; return 0; }\n"));
}

TEST(FlatRule, ACastComparesTheTypeItWasWrittenWith) {
  // A cast keeps its target type off the child list, so comparing class plus
  // children matched `(char)y` as well.
  EXPECT_EQ("int f(int y) { return g((int)y) + (char)y; }\n",
            rewritten("@r@\nexpression E;\n@@\n- (int)E\n+ g((int)E)\n",
                      "int f(int y) { return (int)y + (char)y; }\n"));
}

TEST(FlatRule, SizeofOverATypeComparesThatType) {
  // The operand is a child only when it is an expression, so `sizeof(int)`
  // and `sizeof(long)` were the same node with the same no children.
  EXPECT_EQ("int f(void) { return 4 + sizeof(long); }\n",
            rewritten("@r@\n@@\n- sizeof(int)\n+ 4\n",
                      "int f(void) { return sizeof(int) + sizeof(long); }\n"));
}

TEST(FlatRule, ADeclarationTheComparisonCannotReadIsRefused) {
  // An anonymous tag has no name to compare, and comparing its members
  // instead would accept a different type that happens to agree.
  EXPECT_EQ("!the pattern's declaration holds a Record declarator, which the "
            "comparison has no counterpart for",
            rewritten("@r@\nidentifier x;\n@@\n- struct { int a; } x;\n"
                      "+ int x;\n",
                      "int f(void) { struct { int a; } s; return 0; }\n"));
}

TEST(FlatRule, ShapesOutsideTheFlatPathAreNamedRatherThanRun) {
  // Each of these is a real Coccinelle shape and none is silently
  // approximated, because a rule that half-runs leaves code matching neither
  // the old pattern nor the new one.
  EXPECT_EQ("!a dot-free rule matching a sequence of statements needs "
            "statement adjacency, which this version does not build",
            rewritten("@r@\n@@\n- foo();\n- bar();\n",
                      "void foo(void);\nvoid bar(void);\nvoid f(void) "
                      "{ foo(); bar(); }\n"));
  // A context statement beside a changed one is the same adjacency case: the
  // minus side is a two-statement sequence whichever of the two is marked.
  EXPECT_EQ("!a dot-free rule matching a sequence of statements needs "
            "statement adjacency, which this version does not build",
            rewritten("@r@\n@@\n  foo();\n- bar();\n",
                      "void foo(void);\nvoid bar(void);\nvoid f(void) "
                      "{ foo(); bar(); }\n"));
  EXPECT_EQ("!the rule marks no line, so it asks for no change",
            rewritten("@r@\n@@\n  foo();\n",
                      "void foo(void);\nvoid f(void) { foo(); }\n"));
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

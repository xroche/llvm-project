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
  // An edit the run could not build leaves the output looking right while the
  // rule did something else, so it fails the test rather than passing
  // quietly. Without this a disjunction test could not tell a branch that was
  // excluded from one that matched and had its overlapping edit dropped by
  // `Replacements`, and both tests for branch order passed against an
  // implementation with no cross-branch exclusion at all. An unread branch is
  // not checked here, because a rule with one is still expected to rewrite
  // and `unreadBranchReasons` is what asserts on it.
  if (Result.EditsRefused != 0)
    return "!" + std::to_string(Result.EditsRefused) +
           " edit(s) the run could not build";
  if (Result.Edits.empty())
    return Code.str();
  llvm::Expected<std::string> Out =
      tooling::applyAllReplacements(Code, Result.Edits.begin()->second);
  if (!Out)
    return "!" + llvm::toString(Out.takeError());
  return *Out;
}

/// The reason every rule that could not run gives, one per line.
std::string unrunReasons(llvm::StringRef Patch, llvm::StringRef Code) {
  std::string Error;
  std::optional<SemanticPatch> P = parseSemanticPatch(Patch, "t.cocci", Error);
  if (!P)
    return "!" + Error;
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=gnu11", "-w"}, "input.c");
  if (!Unit)
    return "!no AST";
  RunResult Result;
  runPatch(*P, Unit->getASTContext(), Result);
  std::string Out;
  for (const Unrun &U : Result.UnrunRules)
    Out += U.Reason + "\n";
  return Out;
}

/// The reason every branch that could not be read gives, one per line.
std::string unreadBranchReasons(llvm::StringRef Patch, llvm::StringRef Code) {
  std::string Error;
  std::optional<SemanticPatch> P = parseSemanticPatch(Patch, "t.cocci", Error);
  if (!P)
    return "!" + Error;
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=gnu11", "-w"}, "input.c");
  if (!Unit)
    return "!no AST";
  RunResult Result;
  runPatch(*P, Unit->getASTContext(), Result);
  std::string Out;
  for (const Unrun &U : Result.UnreadBranches)
    Out += U.Reason + "\n";
  return Out;
}

/// What \p Patch over \p Code reported about its own completeness, as
/// `unrun=N unread=N complete=0|1`.
///
/// Spelled out rather than returned as one bool, because a bool any failure
/// satisfies cannot say which failure happened: a patch that did not parse
/// and a rule that ran with one branch unread both read as "not complete".
std::string completeness(llvm::StringRef Patch, llvm::StringRef Code) {
  std::string Error;
  std::optional<SemanticPatch> P = parseSemanticPatch(Patch, "t.cocci", Error);
  if (!P)
    return "!" + Error;
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=gnu11", "-w"}, "input.c");
  if (!Unit)
    return "!no AST";
  RunResult Result;
  runPatch(*P, Unit->getASTContext(), Result);
  return "unrun=" + std::to_string(Result.UnrunRules.size()) +
         " unread=" + std::to_string(Result.UnreadBranches.size()) +
         " complete=" + (Result.complete() ? "1" : "0");
}

/// The line of every finding, in the order the run reported them.
std::string findingLines(llvm::StringRef Patch, llvm::StringRef Code) {
  std::string Error;
  std::optional<SemanticPatch> P = parseSemanticPatch(Patch, "t.cocci", Error);
  if (!P)
    return "!" + Error;
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=gnu11", "-w"}, "input.c");
  if (!Unit)
    return "!no AST";
  RunResult Result;
  runPatch(*P, Unit->getASTContext(), Result);
  std::string Out;
  for (const Finding &F : Result.Findings) {
    if (!Out.empty())
      Out += ",";
    Out += std::to_string(F.Line);
  }
  return Out;
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
  // The space the statement stood in goes with it, so the brace pair is
  // `{ }` and not `{  }`. Measured against `spatch` on this input.
  EXPECT_EQ("void foo(int);\nvoid f(int x) { }\n",
            rewritten("@r@\nexpression E;\n@@\n- foo(E);\n",
                      "void foo(int);\nvoid f(int x) { foo(x); }\n"));
}

// The six tests below pin the whitespace a deletion takes with it. Every
// expected output was read off `spatch` on the same input, because the rule
// is a fit to the reference implementation and not a design of its own. Before
// them the tool made the substantive edit and left the statement's line, its
// indentation, or a newly blank line behind, which was 20 of the 186
// disagreements with Coccinelle's own `.res` files.

TEST(FlatRule, ABracedGroupIsNotAStatementSequence) {
  // `tests/defineinit.cocci`, `tests/substruct.cocci` and `tests/td.cocci`
  // write an initialiser element or a record member between braces, so the
  // `{`, the body and the `}` land as three items. All three used to be
  // refused for missing statement adjacency, and 14 rules of the corpus with
  // a C input were mislabelled the same way.
  EXPECT_EQ("!the `-` side is a brace-delimited group, so its lines are parts "
            "of one construct rather than a sequence of statements, and "
            "matching it needs a pattern for a node inside the braces",
            rewritten("@r@\nexpression E;\n@@\n{\n- .a = E,\n}\n",
                      "struct s { int a; };\nstruct s v = { .a = 1, };\n"));
}

TEST(Whitespace, ADeletedStatementTakesItsWholeLine) {
  EXPECT_EQ("void del(void);\nvoid a(void);\n"
            "void f(void) {\n  a();\n  a();\n}\n",
            rewritten("@r@\n@@\n- del();\n",
                      "void del(void);\nvoid a(void);\n"
                      "void f(void) {\n  a();\n  del();\n  a();\n}\n"));
}

TEST(Whitespace, ADeletionAtTheEndOfABlockTakesTheBlankLineAboveIt) {
  EXPECT_EQ("void del(void);\nvoid a(void);\n"
            "void f(void) {\n  a();\n}\n",
            rewritten("@r@\n@@\n- del();\n",
                      "void del(void);\nvoid a(void);\n"
                      "void f(void) {\n  a();\n\n  del();\n}\n"));
}

TEST(Whitespace, ADeletionAfterTheOpeningBraceTakesTheBlankLineBelowIt) {
  // The rule is directional: above the deletion a blank line survives unless
  // the deletion ends the block, and below it one survives unless the
  // deletion opens the block. Deleting the first statement of a block would
  // otherwise leave a gap under the brace.
  EXPECT_EQ("void del(void);\nvoid a(void);\n"
            "void f(void) {\n  a();\n}\n",
            rewritten("@r@\n@@\n- del();\n",
                      "void del(void);\nvoid a(void);\n"
                      "void f(void) {\n  del();\n\n  a();\n}\n"));
}

TEST(Whitespace, ADeletionFollowedByABlankLineTakesTheBlankLineAboveIt) {
  EXPECT_EQ("void del(void);\nvoid a(void);\n"
            "void f(void) {\n  a();\n\n  a();\n}\n",
            rewritten("@r@\n@@\n- del();\n",
                      "void del(void);\nvoid a(void);\n"
                      "void f(void) {\n  a();\n\n  del();\n\n  a();\n}\n"));
}

TEST(Whitespace, ADeletionSharingItsLineWithKeptCodeKeepsTheLine) {
  EXPECT_EQ("void del(void);\nvoid a(void);\n"
            "void f(void) {\n  a(); a();\n}\n",
            rewritten("@r@\n@@\n- del();\n",
                      "void del(void);\nvoid a(void);\n"
                      "void f(void) {\n  a(); del(); a();\n}\n"));
}

TEST(Whitespace, TwoDeletionsSeparatedByWhitespaceAreWidenedAsOneRegion) {
  // Widening them one at a time keeps the blank line that separated the pair
  // from the statement above, because neither deletion on its own is
  // preceded by a blank line. `tests/argument.cocci` is this shape.
  EXPECT_EQ("void del1(void);\nvoid del2(void);\nvoid a(void);\n"
            "void f(void) {\n  a();\n\n  a();\n}\n",
            rewritten("@r@\n@@\n- del1();\n\n@s@\n@@\n- del2();\n",
                      "void del1(void);\nvoid del2(void);\nvoid a(void);\n"
                      "void f(void) {\n  a();\n\n  del1();\n  del2();\n"
                      "\n  a();\n}\n"));
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

TEST(FlatRule, AnExpressionPatternLeavesTheTerminatorWhereItIs) {
  // `spatch` rewrites `bar(12);` to `4;` here: the `-` side describes the
  // expression and says nothing about the statement around it. Deciding this
  // on the target position alone took the `;` whenever the expression
  // happened to be a statement on its own, which is what
  // `tests/orexp.cocci` disagreed with Coccinelle on.
  EXPECT_EQ("void bar(int);\nvoid f(void) { 4; }\n",
            rewritten("@r@\nexpression F;\n@@\n- bar(F)\n+ 4\n",
                      "void bar(int);\nvoid f(void) { bar(12); }\n"));
}

TEST(FlatRule, AStatementPatternTakesTheTerminatorWithIt) {
  EXPECT_EQ("void bar(int);\nvoid f(void) { 4; }\n",
            rewritten("@r@\nexpression F;\n@@\n- bar(F);\n+ 4;\n",
                      "void bar(int);\nvoid f(void) { bar(12); }\n"));
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

TEST(FlatRule, ADeclarationOutsideAnyFunctionBodyIsMatched) {
  // There is no `DeclStmt` at file scope, so a walk over statements never
  // reaches this declaration. `tests/longlong.cocci` and `tests/cptr.cocci`
  // both rewrote the copy inside `main` and left the file-scope one alone.
  EXPECT_EQ("int a;\nint f(void) { return 0; }\n",
            rewritten("@r@\nidentifier x;\n@@\n- long long x;\n+ int x;\n",
                      "long long a;\nint f(void) { return 0; }\n"));
}

TEST(FlatRule, ATypedefOutsideAnyFunctionBodyIsMatched) {
  // `tests/fntypedef.cocci` is this shape, and a typedef is not a
  // `DeclaratorDecl`, so it needs naming alongside one.
  EXPECT_EQ("typedef void (*t)(int a, int b);\n",
            rewritten("@r@\n@@\n- typedef void (*t)(int a);\n"
                      "+ typedef void (*t)(int a, int b);\n",
                      "typedef void (*t)(int a);\n"));
}

TEST(FlatRule, AFileScopeDeclarationOfTwoThingsIsLeftAlone) {
  // The two declarators share one `;`, so replacing either one of them takes
  // the terminator the other needs.
  EXPECT_EQ("long long a, b;\n",
            rewritten("@r@\nidentifier x;\n@@\n- long long x;\n+ int x;\n",
                      "long long a, b;\n"));
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
  // A `+`-only rule never reaches the runner, because the parser rejects it
  // first with Coccinelle's own wording. The runner's guard for it stays as a
  // guard rather than a reachable path.
  EXPECT_EQ("!t.cocci:1: a '+' slice with no '-' line and no context line: "
            "Coccinelle reports \"minus slice can't be empty\"",
            rewritten("@r@\n@@\n+ foo();\n", "void f(void) { }\n"));
}

TEST(Disjunction, EveryBranchIsTriedAndTheOnesThatMatchApply) {
  // Two branches matching at different sites both fire, and a branch that
  // matches nowhere does not stop the others. `tests/orexp.cocci` is the
  // corpus case, where only the second branch has a site.
  EXPECT_EQ("void foo(int);\nvoid bar(int);\n"
            "void f(void) { 4; 5; }\n",
            rewritten("@r@\nexpression E, F;\n@@\n"
                      "(\n- foo(E)\n+ 4\n|\n- bar(F)\n+ 5\n)\n",
                      "void foo(int);\nvoid bar(int);\n"
                      "void f(void) { foo(1); bar(2); }\n"));
}

TEST(Disjunction, AnEarlierBranchTakesTheTextALaterOneWanted) {
  // The specific branch written first takes the member access, and the
  // general branch does not then also fire on the `p` inside it.
  // `tests/disjexpr.cocci` is the corpus case for this order.
  EXPECT_EQ("struct s { int fld; };\nvoid g(int);\nvoid h(struct s *);\n"
            "void f(struct s *p) { g(p->fld); }\n",
            rewritten("@r@\nidentifier fld; symbol p;\n@@\n"
                      "(\n- p->fld\n+ g(p->fld)\n|\n- p\n+ h(p)\n)\n",
                      "struct s { int fld; };\nvoid g(int);\n"
                      "void h(struct s *);\n"
                      "void f(struct s *p) { p->fld; }\n"));
}

TEST(Disjunction, BranchOrderBeatsNesting) {
  // The same two branches the other way round. Measured on `spatch` 1.1.1:
  // the general branch takes the `p` nested inside the member access, and
  // the specific branch is then left with nothing, so the output keeps the
  // `->fld` it would have replaced. Searching each branch over the whole
  // translation unit in turn is what reproduces this; a walk that offers
  // every branch one site at a time gives the member access to the specific
  // branch instead, because the general one does not match it.
  EXPECT_EQ("struct s { int fld; };\nvoid g(int);\nvoid h(struct s *);\n"
            "void f(struct s *p) { h(p)->fld; }\n",
            rewritten("@r@\nidentifier fld; symbol p;\n@@\n"
                      "(\n- p\n+ h(p)\n|\n- p->fld\n+ g(p->fld)\n)\n",
                      "struct s { int fld; };\nvoid g(int);\n"
                      "void h(struct s *);\n"
                      "void f(struct s *p) { p->fld; }\n"));
}

TEST(Disjunction, MatchesAreReportedInSourceOrderAcrossBranches) {
  // Each branch is searched over the whole translation unit before the next,
  // so the matches come out grouped by branch. A reader looks for them where
  // they are in the file, so they are sorted back into source order. Branch 2
  // here matches the earlier line.
  EXPECT_EQ("4,5", findingLines("@r@\n@@\n(\n- foo();\n+ a();\n|\n- bar();\n"
                                "+ b();\n)\n",
                                "void foo(void);\nvoid bar(void);\n"
                                "void k(void) {\n  bar();\n  foo();\n}\n"));
}

TEST(Disjunction, AFileScopeDeclarationAndTheStatementsInsideItAreOneClaim) {
  // A file-scope declaration is claimed among declarations by identity, and a
  // statement by range, and a declaration's initialiser is in the statement
  // walk. With the two records not consulting each other, a branch matching
  // `1 + 2` and a branch matching the whole declaration both fired on the
  // same text: two edits over overlapping ranges, of which `Replacements`
  // kept whichever it saw first.
  EXPECT_EQ("int x = 7;\n",
            rewritten("@r@\n@@\n(\n- 1 + 2\n+ 7\n|\n- int x = 1 + 2;\n"
                      "+ int x = 8;\n)\n",
                      "int x = 1 + 2;\n"));
  EXPECT_EQ("int x = 8;\n",
            rewritten("@r@\n@@\n(\n- int x = 1 + 2;\n+ int x = 8;\n|\n"
                      "- 1 + 2\n+ 7\n)\n",
                      "int x = 1 + 2;\n"));
}

TEST(Disjunction, ABranchThatCannotBeReadLeavesTheOthersRunning) {
  // `NULL` reaches the pattern parser undeclared, because the target's `NULL`
  // has already been preprocessed and no spelling of it in the synthesised
  // source matches every target. `tests/condexp.cocci` is the corpus case.
  // The other branch still describes sites this patch changes, so it runs.
  EXPECT_EQ("void k(int *);\nvoid g(int *);\nvoid f(int *p) { g(p); }\n",
            rewritten("@r@\nsymbol p;\n@@\n"
                      "(\n- k(NULL)\n+ 0\n|\n- k(p)\n+ g(p)\n)\n",
                      "void k(int *);\nvoid g(int *);\n"
                      "void f(int *p) { k(p); }\n"));
}

TEST(Disjunction, ABranchThatCannotBeReadMakesTheRunIncomplete) {
  // The sites that branch describes are left alone, so the rewrite is partial
  // and must not read as a finished one.
  const llvm::StringRef Patch = "@r@\nsymbol p;\n@@\n"
                                "(\n- k(NULL)\n+ 0\n|\n- k(p)\n+ g(p)\n)\n";
  const llvm::StringRef Code = "void k(int *);\nvoid g(int *);\n"
                               "void f(int *p) { k(p); }\n";
  EXPECT_EQ("unrun=0 unread=1 complete=0", completeness(Patch, Code));
  EXPECT_EQ("branch 1 of the disjunction could not be read, so the sites it "
            "describes are left alone: pattern: the pattern statement did not "
            "parse as C: use of undeclared identifier 'NULL'\n",
            unreadBranchReasons(Patch, Code));
}

TEST(Disjunction, ARuleWhoseEveryBranchIsUnreadableIsRefused) {
  EXPECT_EQ("pattern: the pattern statement did not parse as C: use of "
            "undeclared identifier 'NULL'\n",
            unrunReasons("@r@\n@@\n(\n- k(NULL)\n+ 0\n|\n- m(NULL)\n+ 1\n)\n",
                         "void k(int *);\nvoid f(int *p) { k(p); }\n"));
}

TEST(Disjunction, AnInsertingBranchIsRefusedLikeAnInsertingRule) {
  // `tests/const_adding.cocci` deletes nothing in its first branch and
  // inserts in its second, so the insertion needs a position relative to the
  // match. The refusal names the branch it came from.
  EXPECT_EQ("branch 2 of the disjunction: a dot-free rule that only inserts "
            "needs the insertion placed relative to the match, which this "
            "version does not build\n",
            unrunReasons("@r@\nidentifier I;\n@@\n"
                         "(\n  const int I;\n|\n+ const\n  int I;\n)\n",
                         "void f(void) { const int a; int b; }\n"));
}

TEST(Disjunction, BranchesAskingForDifferentThingsAreRefused) {
  // One branch marks no line, so it only binds, and the other rewrites.
  // Which of the two a site gets is then decided per site rather than per
  // rule. Naming that is what keeps the first branch's purpose from being
  // applied to every site. Both branches are acceptable on their own, which
  // is what makes this reach the agreement check rather than an earlier
  // refusal.
  EXPECT_EQ("the branches of the disjunction ask for different things, so the "
            "rule both rewrites and leaves alone depending on the branch, "
            "which this version does not build\n",
            unrunReasons("@r@\nexpression E;\n@@\n"
                         "(\n  f(E);\n|\n- g(E);\n+ h(E);\n)\n",
                         "void f(int);\nvoid g(int);\nvoid h(int);\n"
                         "void k(void) { f(1); g(2); }\n"));
}

TEST(Disjunction, ADisjunctionInsideALargerPatternIsRefused) {
  // `tests/expopt2.cocci` writes the disjunction between a call's opening and
  // its closing parenthesis, so its branches are nodes inside a pattern
  // rather than the pattern itself.
  EXPECT_EQ("a disjunction that is not the whole rule body needs its branches "
            "matched inside a larger pattern, which this version does not "
            "build\n",
            unrunReasons("@r@\nidentifier fld; symbol v;\n@@\n"
                         " f(v,\n(\n- v.fld\n+ 1\n|\n- v.other\n+ 2\n)\n )\n",
                         "void f(int, int);\nvoid g(void) { }\n"));
}

TEST(Inherited, ARuleThatMarksNoLineRunsForTheValuesItBinds) {
  // It changes nothing, and refusing it left a rule declaring `r.E` with
  // nothing to constrain it.
  EXPECT_EQ("void foo(int);\nvoid f(void) { foo(1); }\n",
            rewritten("@r@\nexpression E;\n@@\n  foo(E);\n",
                      "void foo(int);\nvoid f(void) { foo(1); }\n"));
}

TEST(Inherited, AnInheritedMetavariableTakesOnlyTheValuesTheEarlierRuleBound) {
  // Measured against `spatch` 1.1.1 on this input: it rewrites `h(1)` and
  // leaves `h(2)` alone. Before inheritance was honoured, `expression r.X`
  // matched any expression and both calls were rewritten.
  EXPECT_EQ("void f(int);\nvoid h(int);\nvoid hh(int);\n"
            "void g(void) { f(1); hh(1); h(2); }\n",
            rewritten("@r@\nexpression X;\n@@\n  f(X);\n"
                      "\n@@\nexpression r.X;\n@@\n- h(X);\n+ hh(X);\n",
                      "void f(int);\nvoid h(int);\nvoid hh(int);\n"
                      "void g(void) { f(1); h(1); h(2); }\n"));
}

TEST(Inherited, EveryValueTheEarlierRuleBoundIsTried) {
  // `tests/skip.cocci` is the corpus case: one rule binds `E` twice over and
  // the rule inheriting it has to delete both sites, so running the dependent
  // rule once per environment is what the expected output needs.
  EXPECT_EQ("void f(int);\nvoid g(void) { }\n",
            rewritten("@r@\nexpression E;\n@@\n  f(E)\n"
                      "\n@@\nexpression r.E;\n@@\n- f(E);\n",
                      "void f(int);\nvoid g(void) { f(1); f(2); }\n"));
}

TEST(Inherited, ARuleInheritingFromOneThatDidNotRunIsRefused) {
  // The values the declaration admits are unknown, and running the rule
  // anyway is what rewrote more than the patch asked for. The binding rule
  // here matches two adjacent statements, which the flat path does not build.
  EXPECT_EQ("a dot-free rule matching a sequence of statements needs "
            "statement adjacency, which this version does not build\n"
            "the rule inherits a metavariable from rule 'r', which did not "
            "run, so the values that metavariable may take are unknown and "
            "matching without them would rewrite more than the patch asks "
            "for\n",
            unrunReasons("@r@\nexpression X;\n@@\n f(X);\n g(X);\n"
                         "\n@@\nexpression r.X;\n@@\n- h(X);\n+ hh(X);\n",
                         "void f(int);\nvoid g(int);\nvoid h(int);\n"
                         "void k(void) { g(1); h(2); }\n"));
}

TEST(Inherited, ARuleThatRanAndBoundNothingLeavesTheDependentRuleWithNoSites) {
  // Distinct from the refusal above: the constraint is known and admits
  // nothing, so the dependent rule runs and matches nowhere.
  EXPECT_EQ("void f(int);\nvoid h(int);\nvoid k(void) { h(2); }\n",
            rewritten("@r@\nexpression X;\n@@\n  f(X);\n"
                      "\n@@\nexpression r.X;\n@@\n- h(X);\n+ hh(X);\n",
                      "void f(int);\nvoid h(int);\nvoid k(void) { h(2); }\n"));
}

TEST(Inherited, ATypeMetavariableNamingATypedefBindsTheNameItIntroduces) {
  // `tests/typedef2.cocci` declares `type t, s;` and writes `typedef t s@p;`,
  // where `s` stands for the alias rather than for a type written out. The
  // dependent rule then rewrites one line per alias.
  EXPECT_EQ("typedef int A, B;\nvoid f(void) { int x; int y; }\n",
            rewritten("@r@\ntype t, s;\n@@\n  typedef t s;\n"
                      "\n@@\ntype r.s;\nsymbol x, y;\n@@\n- s\n+ int\n"
                      "  x;\n",
                      "typedef int A, B;\nvoid f(void) { A x; int y; }\n"));
}

TEST(Inherited, EveryDeclaratorOfOneTypedefBindsTheNameItIntroduces) {
  // Clang gives every declarator of one declaration the same begin location,
  // so `typedef int A, B;` is two declarations over one source range. A
  // search that excludes an already-matched range rather than an
  // already-matched declaration lets the first of them hide the second, and
  // `tests/typedef2.cocci` then rewrites one of its four lines instead of all
  // four.
  EXPECT_EQ("typedef int A, B;\nvoid f(void) { int x; int y; }\n",
            rewritten("@r@\ntype t, s;\n@@\n  typedef t s;\n"
                      "\n@@\ntype r.s;\nidentifier v;\n@@\n- s\n+ int\n"
                      "  v;\n",
                      "typedef int A, B;\nvoid f(void) { A x; B y; }\n"));
}

TEST(FlatRule, ADeclaredTypeNameIsAvailableToEveryRuleOfThePatch) {
  // `typedef X;` used to suppress the undeclared-name refusal and declare
  // nothing, so the pattern reached Clang with an undeclared type and failed
  // to parse while the refusal that would have said so was gone. The name is
  // declared for the whole patch, which is where Coccinelle keeps it and what
  // `tests/wchar.cocci` needs: it declares in the first rule and writes in
  // the second.
  EXPECT_EQ("typedef int myint;\nvoid f(void) { }\n",
            rewritten("@r@\ntypedef myint;\n@@\n  myint a;\n"
                      "\n@@\nidentifier v;\n@@\n- myint v = 0;\n",
                      "typedef int myint;\n"
                      "void f(void) { myint v = 0; }\n"));
}

TEST(FlatRule, ATypeNameWrittenAloneIsATypePatternAndIsRefused) {
  // `tests/compare.cocci`, `tests/devlink.cocci`, `tests/macro.cocci` and
  // `tests/weirdinit_failure.cocci` each write a bare type name as their
  // whole `-` side and mean the type. Each of them used to run: the name was
  // undeclared, so it was synthesised as a variable and the pattern read as
  // an expression referring to one, which matched nothing the patch meant.
  // Declaring the name makes the line declare nothing, which is what it is.
  EXPECT_EQ("pattern: the pattern declares nothing, so it is a type rather "
            "than a statement\n",
            unrunReasons("@r@\ntypedef mytype;\n@@\n- mytype\n"
                         "+ struct other\n",
                         "typedef int mytype;\nvoid f(mytype *p) { }\n"));
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

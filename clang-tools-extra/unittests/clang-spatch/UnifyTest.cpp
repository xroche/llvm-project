//===--- UnifyTest.cpp - Tests for AST-against-AST matching --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Unify.h"
#include "Edit.h"
#include "clang/Tooling/Tooling.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace clang::spatch {
namespace {

MetaVar mv(enum MetaVar::Kind K, llvm::StringRef Name) {
  return MetaVar{K, Name.str(), {}};
}

/// Matches \p Pattern against \p Code and returns one line per match, listing
/// the matched text and each binding, or "!" plus the reason none was tried.
///
/// \p BraceGroup says the pattern is one brace group, which is what grouping
/// tells the pattern parser and what the text alone cannot say.
std::vector<std::string> matches(llvm::StringRef Pattern,
                                 std::vector<MetaVar> MetaVars,
                                 llvm::StringRef Code,
                                 bool BraceGroup = false) {
  std::vector<std::string> Out;
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=gnu11", "-w"}, "input.c");
  if (!Unit)
    return {"!no AST"};
  std::string Error;
  std::optional<ParsedPattern> P =
      parsePattern(MetaVars, {Pattern.str()}, {}, {BraceGroup}, Error);
  if (!P)
    return {"!" + Error};
  if (!P->Items[0])
    return {"!" + P->Errors[0]};
  if (std::string Why = whyNotComparable(P->Items[0]); !Why.empty())
    return {"!" + Why};
  ASTContext &Ctx = Unit->getASTContext();
  for (const Match &M : findMatches(P->Items[0], *P, Ctx)) {
    std::string Line = sourceTextOf(M.Node.getSourceRange(), Ctx).str();
    std::vector<std::string> Names;
    for (const auto &B : M.Bound)
      Names.push_back(
          (B.first() + "=" + sourceTextOf(B.second.Range, Ctx)).str());
    llvm::sort(Names);
    for (const std::string &N : Names)
      Line += " {" + N + "}";
    Out.push_back(Line);
  }
  return Out;
}

using Strings = std::vector<std::string>;

/// Every pattern node whose written text is \p Wanted, paired with the text
/// and file offset of the target node it matched.
///
/// The offset is what makes the test meaningful: two target nodes can have
/// the same text, and pairing them by text is exactly the mistake a token
/// diff makes.
std::vector<std::string> pairedWith(llvm::StringRef Pattern,
                                    std::vector<MetaVar> MetaVars,
                                    llvm::StringRef Code,
                                    llvm::StringRef Wanted) {
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=gnu11", "-w"}, "input.c");
  if (!Unit)
    return {"!no AST"};
  std::string Error;
  std::optional<ParsedPattern> P =
      parsePattern(MetaVars, {Pattern.str()}, {}, {}, Error);
  if (!P || !P->Items[0])
    return {"!" + (P ? P->Errors[0] : Error)};
  ASTContext &Ctx = Unit->getASTContext();
  ASTContext &PatCtx = P->Unit->getASTContext();
  MatchOptions Opts;
  Opts.WantNodePairs = true;
  std::vector<std::string> Out;
  for (const Match &M : findMatches(P->Items[0], *P, Ctx, Opts)) {
    for (const auto &Pair : M.Pairs) {
      if (sourceTextOf(*Pair.Pattern, PatCtx) != Wanted)
        continue;
      const SourceLocation Begin = Pair.Target->getSourceRange().getBegin();
      Out.push_back((sourceTextOf(*Pair.Target, Ctx) + "@" +
                     std::to_string(Ctx.getSourceManager().getFileOffset(Begin)))
                        .str());
    }
  }
  return Out;
}

} // namespace

TEST(Unify, EveryStatementFormMatchesNotJustACall) {
  // The reason this exists. 620 pattern sites in the sample corpus reported
  // `only a call statement is supported as an anchor`.
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "E")};
  EXPECT_EQ(Strings({"a = 1 {E=a}"}),
            matches("E = 1;", E, "void f(void) { int a; a = 1; }\n"));
  EXPECT_EQ(Strings({"return x + 1 {E=x + 1}"}),
            matches("return E;", E, "int f(int x) { return x + 1; }\n"));
  EXPECT_EQ(Strings({"if (c) g()"}),
            matches("if (c) g();", {},
                    "int c; void g(void);\nvoid f(void) { if (c) g(); }\n"));
  EXPECT_EQ(Strings({"while (c) g()"}),
            matches("while (c) g();", {},
                    "int c; void g(void);\nvoid f(void) { while (c) g(); }\n"));
  EXPECT_EQ(Strings({"a + 1 {E=a}"}),
            matches("E + 1;", E, "void f(void) { int a; a + 1; }\n"));
}

TEST(Unify, AMetavariableBindsAndMustBindTheSameThingTwice) {
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "E")};
  // Both positions are the same expression, so the pattern matches.
  EXPECT_EQ(Strings({"a = a {E=a}"}),
            matches("E = E;", E, "void f(void) { int a; a = a; }\n"));
  // Different expressions, so it must not.
  EXPECT_EQ(Strings({}),
            matches("E = E;", E, "void f(void) { int a, b; a = b; }\n"));
}

TEST(Unify, TheSameNameInTwoFunctionsIsTwoDeclarations) {
  // A text pattern cannot tell these apart. Binding compares declarations, so
  // `l` in one function is not the `l` of another.
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "E")};
  EXPECT_EQ(Strings({"g(l, l) {E=l}"}),
            matches("g(E, E);", E,
                    "void g(int, int);\n"
                    "void f(int l) { g(l, l); }\n"
                    "void h(int l, int m) { g(l, m); }\n"));
}

TEST(Unify, AConstantMetavariableTakesALiteralAndNotAVariable) {
  const std::vector<MetaVar> C = {mv(MetaVar::Kind::Constant, "C")};
  EXPECT_EQ(Strings({"g(12) {C=12}"}),
            matches("g(C);", C, "void g(int);\nvoid f(int x) { g(12); }\n"));
  EXPECT_EQ(Strings({}),
            matches("g(C);", C, "void g(int);\nvoid f(int x) { g(x); }\n"));
}

TEST(Unify, AnOperatorIsPartOfTheNodesIdentity) {
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "E")};
  // `a - 1` must not match a pattern written `E + 1`.
  EXPECT_EQ(Strings({}),
            matches("E + 1;", E, "void f(void) { int a; a - 1; }\n"));
}

TEST(Unify, ArgumentEllipsisTakesItsFourShapes) {
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "E")};
  const llvm::StringRef Code = "void g(int, int, int);\n"
                               "void f(int a, int b, int c) { g(a, b, c); }\n";
  EXPECT_EQ(Strings({"g(a, b, c)"}), matches("g(...);", {}, Code));
  EXPECT_EQ(Strings({"g(a, b, c) {E=a}"}), matches("g(E, ...);", E, Code));
  EXPECT_EQ(Strings({"g(a, b, c) {E=c}"}), matches("g(..., E);", E, Code));
  EXPECT_EQ(Strings({"g(a, b, c) {E=a}"}), matches("g(..., E, ...);", E, Code));
}

TEST(Unify, ACallInsideAMatchIsNotReportedTwice) {
  // One written call is one match, so a pattern is not also reported against
  // a subtree of something it already matched.
  EXPECT_EQ(Strings({"g(g(1))"}),
            matches("g(...);", {}, "int g(int);\nvoid f(void) { g(g(1)); }\n"));
}

TEST(Unify, AMemberAccessComparesTheMemberAndTheArrow) {
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "E")};
  const llvm::StringRef Code = "struct S { int a; int b; };\n"
                               "void g(int);\n"
                               "void f(struct S *p) { g(p->a); }\n";
  EXPECT_EQ(Strings({"g(p->a) {E=p}"}), matches("g(E->a);", E, Code));
  EXPECT_EQ(Strings({}), matches("g(E->b);", E, Code));
  // A dot is not an arrow.
  EXPECT_EQ(Strings({}), matches("g(E.a);", E, Code));
}

TEST(Unify, TheEditTargetIsTheOccurrenceThePatternMarksAndNotTheFirstAlike) {
  // The case that rules out diffing the two sides as token sequences. Both
  // arguments are spelled `x`, so an alignment by text or a
  // longest-common-subsequence diff pairs the pattern's `x` with the first
  // one. `spatch` rewrites the second, which is the one the pattern marks.
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "e")};
  EXPECT_EQ(Strings({"x@23"}),
            pairedWith("g(e, x);", E, "void m() { int x; g(x, x); }", "x"));
  // And when the binding itself holds a token equal to the marked one.
  EXPECT_EQ(Strings({"x@34"}),
            pairedWith("g(e, x);", E, "void m() { int x, a; g(a + x * x, x); }",
                       "x"));
}

TEST(Unify, APatternNodePairsWithTheTargetNodeAtTheSameDepth) {
  // The mark is on the outer argument in the first pattern and on the inner
  // one in the second, over the same target. `spatch` follows the mark, so
  // the depth of the pattern node and not the spelling of the token decides.
  const std::vector<MetaVar> Ids = {mv(MetaVar::Kind::Identifier, "y")};
  const char *Code = "void g(int, int); int h(int);\n"
                     "void m() { int a; g(a, h(a)); }";
  EXPECT_EQ(Strings({"a@50"}), pairedWith("g(y, h(a));", Ids, Code, "y"));
  EXPECT_EQ(Strings({"a@55"}), pairedWith("g(a, h(y));", Ids, Code, "y"));
}

TEST(Unify, APatternsOwnParenthesesArePairedRatherThanPeeledAway) {
  // `binop` writes `- (i = i2)` and its expected output drops the target's
  // parentheses with it, so the pair has to be the target's ParenExpr and not
  // the assignment inside it.
  const std::vector<MetaVar> Ids = {mv(MetaVar::Kind::Identifier, "i"),
                                    mv(MetaVar::Kind::Identifier, "i2")};
  EXPECT_EQ(Strings({"(i = j)@25"}),
            pairedWith("if ((i = i2) + 0) { }", Ids,
                       "void m() { int i, j; if ((i = j) + 0) { } }",
                       "(i = i2)"));
}

TEST(Unify, NoPairsAreRecordedUnlessTheCallerAsksForThem) {
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "e")};
  std::unique_ptr<ASTUnit> Unit = tooling::buildASTFromCodeWithArgs(
      "void m() { g(1); }", {"-std=gnu11", "-w"}, "input.c");
  std::string Error;
  std::optional<ParsedPattern> P = parsePattern(E, {"g(e);"}, {}, {}, Error);
  ASSERT_TRUE(P.has_value()) << Error;
  std::vector<Match> M = findMatches(P->Items[0], *P, Unit->getASTContext());
  ASSERT_EQ(1u, M.size());
  EXPECT_TRUE(M[0].Pairs.empty());
}

TEST(Unify, AWindowThatFailsLeavesNoPairsBehindItsSuccessor) {
  // `f(..., E, ...)` puts its named term at no fixed position, so every
  // window it could occupy is tried and the match as a whole still succeeds
  // after a failure. That is the one failure inside a match that does not
  // reach the caller, so the window has to undo its own pairs.
  const std::vector<MetaVar> None;
  EXPECT_EQ(Strings({"7@19"}),
            pairedWith("f(..., 7, ...);", None,
                       "void m() { f(1, 8, 7, 9); }", "7"));
}

TEST(Unify, PairsDoNotAccumulateAcrossCandidates) {
  // A candidate that fails is compared node by node and gets some way in
  // before it fails, so its pairs would be read as the next candidate's own.
  // The two calls before the matching one each get as far as their second
  // argument, which is the position the pattern marks.
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "e")};
  EXPECT_EQ(Strings({"7@34"}),
            pairedWith("g(e, 7);", E,
                       "void m() { g(1, 8); g(2, 9); g(3, 7); }", "7"));
}

TEST(Unify, ABlockAsTheWholeMinusSideIsRefusedAndOneInsideAPatternIsNot) {
  // `spatch` 1.1.1 changes nothing for `- { foo(); }` and leaves a function's
  // own body alone even for the multi-line shape `braces.cocci` writes, while
  // the walk here offers every `CompoundStmt` including a function body. So
  // `int main() { foo(); }` came back as `int main() foo();` at exit 0.
  const std::vector<MetaVar> None;
  std::string Error;
  std::optional<ParsedPattern> P =
      parsePattern(None, {"{ 1; }", "if (1) { 2; }"}, {}, {}, Error);
  ASSERT_TRUE(P.has_value()) << Error;
  ASSERT_TRUE(P->Items[0] != nullptr) << P->Errors[0];
  ASSERT_TRUE(P->Items[1] != nullptr) << P->Errors[1];
  EXPECT_THAT(whyNotComparable(P->Items[0]),
              testing::HasSubstr("the `-` side is a block"));
  EXPECT_EQ("", whyNotComparable(P->Items[1]));
}

TEST(Unify, AOneElementBraceGroupMatchesThatElementWhereverItStands) {
  // `spatch` 1.1.1 rewrites the `.a = 7,` of `{ .a = 7, .c = 8, }` and leaves
  // the rest, so the group is not the whole list. It matches one nested in
  // another list and one in a compound literal, and it refuses a different
  // designator, an array designator against a field one, and an element with
  // no designator at all.
  const std::vector<MetaVar> E = {mv(MetaVar::Kind::Expression, "E")};
  EXPECT_EQ(Strings({".a = 7 {E=7}"}),
            matches("{ .a = E, }", E,
                    "struct a { int a; int c; } y = { .a = 7, .c = 8, };\n",
                    /*BraceGroup=*/true));
  EXPECT_EQ(Strings({}), matches("{ .a = E, }", E,
                                 "struct a { int b; } y = { .b = 7, };\n",
                                 /*BraceGroup=*/true));
  EXPECT_EQ(Strings({}), matches("{ .a = E, }", E,
                                 "struct a { int a; } y = { 7, };\n",
                                 /*BraceGroup=*/true));
  EXPECT_EQ(Strings({}), matches("{ .a = E, }", E, "int y[2] = { [0] = 7, };\n",
                                 /*BraceGroup=*/true));
}

} // namespace clang::spatch

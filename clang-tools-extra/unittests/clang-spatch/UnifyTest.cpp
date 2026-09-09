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
#include "gtest/gtest.h"

namespace clang::spatch {
namespace {

MetaVar mv(enum MetaVar::Kind K, llvm::StringRef Name) {
  return MetaVar{K, Name.str(), {}};
}

/// Matches \p Pattern against \p Code and returns one line per match, listing
/// the matched text and each binding, or "!" plus the reason none was tried.
std::vector<std::string> matches(llvm::StringRef Pattern,
                                 std::vector<MetaVar> MetaVars,
                                 llvm::StringRef Code) {
  std::vector<std::string> Out;
  std::unique_ptr<ASTUnit> Unit =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=gnu11", "-w"}, "input.c");
  if (!Unit)
    return {"!no AST"};
  std::string Error;
  std::optional<ParsedPattern> P =
      parsePattern(MetaVars, {Pattern.str()}, {}, Error);
  if (!P)
    return {"!" + Error};
  if (!P->Items[0])
    return {"!" + P->Errors[0]};
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

} // namespace clang::spatch

//===--- PatternParserTest.cpp - Tests for the pattern parser ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PatternParser.h"
#include "clang/AST/Expr.h"
#include "clang/AST/StmtVisitor.h"
#include "gtest/gtest.h"

namespace clang::spatch {
namespace {

MetaVar mv(enum MetaVar::Kind K, llvm::StringRef Name) {
  return MetaVar{K, Name.str(), {}};
}

/// The parsed items, or the per-item error prefixed by "!".
std::vector<std::string> parsedKinds(llvm::ArrayRef<MetaVar> MetaVars,
                                     llvm::ArrayRef<std::string> Stmts) {
  std::string Error;
  std::optional<ParsedPattern> P = parsePattern(MetaVars, Stmts, Error);
  std::vector<std::string> Out;
  if (!P) {
    Out.push_back("!" + Error);
    return Out;
  }
  for (unsigned I = 0; I != Stmts.size(); ++I)
    Out.push_back(P->Items[I] ? P->Items[I]->getStmtClassName()
                              : "!" + P->Errors[I]);
  return Out;
}

} // namespace

TEST(SynthesiseDeclarations, ABareMetavariableTakesAnyType) {
  const std::vector<MetaVar> MV = {mv(MetaVar::Kind::Expression, "E")};
  const std::string S = synthesiseDeclarations(MV, {"foo(E);"});
  EXPECT_NE(std::string::npos, S.find("extern int E;")) << S;
}

TEST(SynthesiseDeclarations, AMemberAccessBuildsATypeThatHasTheMember) {
  // `int` cannot be dereferenced, so a pattern using `x->y` needs a type
  // carrying `y`. Clang reports `member reference type 'int' is not a
  // pointer` otherwise, and the whole rule then fails to parse.
  const std::vector<MetaVar> MV = {mv(MetaVar::Kind::Expression, "x")};
  const std::string S = synthesiseDeclarations(MV, {"x->y = 0;"});
  EXPECT_NE(std::string::npos, S.find("struct __spatch_x_t { int y; };")) << S;
  EXPECT_NE(std::string::npos, S.find("struct __spatch_x_t *x;")) << S;
}

TEST(SynthesiseDeclarations, EveryMemberTheRuleNamesGoesIntoTheType) {
  const std::vector<MetaVar> MV = {mv(MetaVar::Kind::Expression, "p")};
  const std::string S = synthesiseDeclarations(MV, {"p->a = 1;", "p->b = 2;"});
  EXPECT_NE(std::string::npos, S.find("int a; int b;")) << S;
}

TEST(SynthesiseDeclarations, ADotAccessIsNotAPointer) {
  const std::vector<MetaVar> MV = {mv(MetaVar::Kind::Expression, "s")};
  const std::string S = synthesiseDeclarations(MV, {"s.f = 0;"});
  EXPECT_NE(std::string::npos, S.find("struct __spatch_s_t s;")) << S;
  EXPECT_EQ(std::string::npos, S.find("_t *s;")) << S;
}

TEST(SynthesiseDeclarations, ATypeMetavariableBecomesATypedef) {
  const std::vector<MetaVar> MV = {mv(MetaVar::Kind::Type, "T")};
  EXPECT_NE(std::string::npos,
            synthesiseDeclarations(MV, {"T x;"}).find("typedef int T;"));
}

TEST(SynthesiseDeclarations, ANameInsideALongerNameIsADifferentName) {
  // Scanning for `E` must not fire on `Extra`, which would give the pattern a
  // type derived from a use that is not its own.
  const std::vector<MetaVar> MV = {mv(MetaVar::Kind::Expression, "E")};
  const std::string S = synthesiseDeclarations(MV, {"foo(Extra->m, E);"});
  EXPECT_NE(std::string::npos, S.find("extern int E;")) << S;
  EXPECT_EQ(std::string::npos, S.find("__spatch_E_t")) << S;
}

TEST(ParsePattern, EveryStatementFormTheCorpusDemandsParses) {
  const std::vector<MetaVar> MV = {
      mv(MetaVar::Kind::Expression, "E"), mv(MetaVar::Kind::Expression, "F"),
      mv(MetaVar::Kind::Identifier, "fn"), mv(MetaVar::Kind::Type, "T")};
  // The forms are the ones the old compiler turned away, in the order of how
  // often the corpus asks for them.
  EXPECT_EQ(std::vector<std::string>({"CallExpr"}),
            parsedKinds(MV, {"fn(E, F);"}));
  EXPECT_EQ(std::vector<std::string>({"BinaryOperator"}),
            parsedKinds(MV, {"E = F;"}));
  EXPECT_EQ(std::vector<std::string>({"DeclStmt"}), parsedKinds(MV, {"T x;"}));
  EXPECT_EQ(std::vector<std::string>({"IfStmt"}),
            parsedKinds(MV, {"if (E) F;"}));
  EXPECT_EQ(std::vector<std::string>({"WhileStmt"}),
            parsedKinds(MV, {"while (E) F;"}));
  EXPECT_EQ(std::vector<std::string>({"ReturnStmt"}),
            parsedKinds(MV, {"return E;"}));
  EXPECT_EQ(std::vector<std::string>({"BinaryOperator"}),
            parsedKinds(MV, {"E + F;"}));
}

TEST(ParsePattern, AMetavariableReferenceResolvesToADeclarationWeOwn) {
  // This is what makes a wildcard a pointer comparison rather than a name
  // comparison, so a target function that happens to be called `E` is not
  // mistaken for the metavariable.
  const std::vector<MetaVar> MV = {mv(MetaVar::Kind::Expression, "E")};
  std::string Error;
  std::optional<ParsedPattern> P = parsePattern(MV, {"foo(E);"}, Error);
  ASSERT_TRUE(P.has_value()) << Error;
  ASSERT_NE(nullptr, P->Items[0]);
  const auto *Call = dyn_cast<CallExpr>(P->Items[0]);
  ASSERT_NE(nullptr, Call);
  ASSERT_EQ(1u, Call->getNumArgs());
  const auto *Ref =
      dyn_cast<DeclRefExpr>(Call->getArg(0)->IgnoreParenImpCasts());
  ASSERT_NE(nullptr, Ref);
  const MetaVar *M = P->metaVarFor(Ref->getDecl()->getCanonicalDecl());
  ASSERT_NE(nullptr, M);
  EXPECT_EQ("E", M->Name);
}

TEST(ParsePattern, AnArgumentEllipsisSurvivesAsAMarkerCall) {
  std::string Error;
  std::optional<ParsedPattern> P = parsePattern({}, {"foo(...);"}, Error);
  ASSERT_TRUE(P.has_value()) << Error;
  ASSERT_NE(nullptr, P->Items[0]);
  const auto *Call = dyn_cast<CallExpr>(P->Items[0]);
  ASSERT_NE(nullptr, Call);
  ASSERT_EQ(1u, Call->getNumArgs());
  const auto *Inner =
      dyn_cast<CallExpr>(Call->getArg(0)->IgnoreParenImpCasts());
  ASSERT_NE(nullptr, Inner);
  ASSERT_NE(nullptr, Inner->getDirectCallee());
  EXPECT_EQ(DotsMarker, Inner->getDirectCallee()->getName());
}

TEST(ParsePattern, OneStatementThatDoesNotParseLeavesTheOthersAlone) {
  // A rule is refused per statement rather than wholesale, because one
  // unsupported line must not decide the fate of the lines around it.
  const std::vector<MetaVar> MV = {mv(MetaVar::Kind::Expression, "E")};
  const std::vector<std::string> Got =
      parsedKinds(MV, {"foo(E);", "this is not C ###", "bar(E);"});
  ASSERT_EQ(3u, Got.size());
  EXPECT_EQ("CallExpr", Got[0]);
  EXPECT_EQ('!', Got[1][0]) << Got[1];
  EXPECT_EQ("CallExpr", Got[2]);
}

} // namespace clang::spatch

//===--- SmplParserTest.cpp - Tests for the SmPL parser ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Four tiers, and the last one is the point of the file.
//
//  1. Hand-written cases, one per grammar production and one per lexical trap.
//  2. Real kernel rules that sit inside the subset, asserted on structure.
//  3. Real kernel rules that sit outside it, asserted on refusal names.
//  4. A sweep over all 1209 native samples in test/Inputs/cocci. Every file
//     has to land in exactly one of three states and never a fourth: parsed
//     clean, parsed with every unsupported construct named in Refusals, or
//     rejected with an error. Zero crashes, zero timeouts, and zero files that
//     report success while holding a construct the parser did not understand.
//
// The expected outcome per file comes from test/Inputs/corpus-manifest.tsv
// rather than from a classification made here.
//
//===----------------------------------------------------------------------===//

#include "SmplParser.h"
#include "PatternParser.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;

namespace clang::spatch {
namespace {

using ItemKind = decltype(PatternItem::Kind);
using ItemMarker = decltype(PatternItem::Marker);
using MetaKind = decltype(MetaVar::Kind);

/// Parses \p Text, failing the test if the parser rejects it.
SemanticPatch parsed(StringRef Text) {
  std::string Error;
  auto P = parseSemanticPatch(Text, "t.cocci", Error);
  EXPECT_TRUE(P.has_value()) << "unexpected rejection: " << Error;
  return P ? std::move(*P) : SemanticPatch();
}

/// The error text from a parse that has to fail.
std::string rejection(StringRef Text) {
  std::string Error;
  auto P = parseSemanticPatch(Text, "t.cocci", Error);
  EXPECT_FALSE(P.has_value()) << "expected a rejection, got a patch";
  return Error;
}

bool refused(const SemanticPatch &P, StringRef Construct) {
  for (const Refusal &R : P.Refusals)
    if (R.Construct == Construct)
      return true;
  return false;
}

/// Every refusal name, for a failure message.
std::string refusalList(const SemanticPatch &P) {
  std::string Out;
  for (const Refusal &R : P.Refusals) {
    if (!Out.empty())
      Out += "; ";
    Out += R.Construct;
  }
  return Out.empty() ? "<none>" : Out;
}

#define EXPECT_REFUSED(Patch, Name)                                            \
  EXPECT_TRUE(refused((Patch), (Name)))                                        \
      << "missing refusal '" << (Name) << "'; got: " << refusalList(Patch)

#define EXPECT_NOT_REFUSED(Patch, Name)                                        \
  EXPECT_FALSE(refused((Patch), (Name)))                                       \
      << "unexpected refusal '" << (Name) << "'"

/// Every refusal carries a construct name, a reason and a line.
void expectWellFormedRefusals(const SemanticPatch &P) {
  for (const Refusal &R : P.Refusals) {
    EXPECT_FALSE(R.Construct.empty()) << "a refusal with no construct name";
    EXPECT_FALSE(R.Reason.empty())
        << "refusal '" << R.Construct << "' carries no reason";
    EXPECT_NE(0u, R.Line) << "refusal '" << R.Construct << "' has no line";
  }
}

std::string testInputsDir() {
  if (const char *Env = std::getenv("CLANG_SPATCH_TEST_INPUTS"))
    return Env;
#ifdef CLANG_SPATCH_TEST_INPUTS
  return CLANG_SPATCH_TEST_INPUTS;
#else
  SmallString<256> Dir(sys::path::parent_path(__FILE__));
  sys::path::append(Dir, "test", "Inputs");
  return std::string(Dir);
#endif
}

/// Reads one file of the checked-in corpus, by its manifest-relative path.
std::string readInput(StringRef Name) {
  SmallString<256> Path(testInputsDir());
  sys::path::append(Path, Name);
  auto Buf = MemoryBuffer::getFile(Path);
  EXPECT_TRUE(static_cast<bool>(Buf))
      << "cannot read " << Path.c_str()
      << "; set CLANG_SPATCH_TEST_INPUTS to the test/Inputs directory";
  return Buf ? (*Buf)->getBuffer().str() : std::string();
}

} // namespace

//===----------------------------------------------------------------------===//
// Tier 1: hand-written cases, one per production and one per lexical trap.
//===----------------------------------------------------------------------===//

TEST(SmplParser, NotASemanticPatch) {
  std::string Error;
  EXPECT_FALSE(
      parseSemanticPatch("// just a comment\n", "t.cocci", Error).has_value());
  EXPECT_NE(std::string::npos, Error.find("no rule header"));
  Error.clear();
  EXPECT_FALSE(
      parseSemanticPatch("int main(void) { return 0; }\n", "t.c", Error)
          .has_value());
  EXPECT_NE(std::string::npos, Error.find("stray text"));
}

TEST(SmplParser, UnnamedHeader) {
  SemanticPatch P = parsed("@@\nexpression E;\n@@\n- foo(E);\n");
  ASSERT_EQ(1u, P.Rules.size());
  EXPECT_TRUE(P.Rules[0].Name.empty());
  EXPECT_FALSE(P.Rules[0].Quant.has_value());
  ASSERT_EQ(1u, P.Rules[0].Body.size());
  EXPECT_EQ("foo(E);", P.Rules[0].Body[0].Text);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
}

TEST(SmplParser, DeclarationsOnTheHeaderLine) {
  // `@@ statement s; @@` is legal SmPL that a line-oriented reader can drop.
  SemanticPatch P = parsed("@@ statement s; @@\n- foo();\n  s\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].MetaVars.size());
  EXPECT_EQ("s", P.Rules[0].MetaVars[0].Name);
  EXPECT_EQ(2u, P.Rules[0].Body.size());
}

TEST(SmplParser, NamedHeaderAndQuantifiers) {
  SemanticPatch P = parsed("@r1 exists@\n@@\n* foo();\n\n"
                           "@r2 depends on r1 forall@\n@@\n* bar();\n");
  ASSERT_EQ(2u, P.Rules.size());
  EXPECT_EQ("r1", P.Rules[0].Name);
  ASSERT_TRUE(P.Rules[0].Quant.has_value());
  EXPECT_EQ(Rule::Quantifier::Exists, *P.Rules[0].Quant);
  EXPECT_EQ("r2", P.Rules[1].Name);
  EXPECT_EQ("r1", P.Rules[1].DependsOn);
  EXPECT_FALSE(P.Rules[1].DependsOnVirtual);
  ASSERT_TRUE(P.Rules[1].Quant.has_value());
  EXPECT_EQ(Rule::Quantifier::Forall, *P.Rules[1].Quant);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
}

TEST(SmplParser, MultiLineHeader) {
  SemanticPatch P = parsed("@ r\nexists\n@\nexpression E;\n@@\n* foo(E);\n");
  ASSERT_EQ(1u, P.Rules.size());
  EXPECT_EQ("r", P.Rules[0].Name);
  ASSERT_TRUE(P.Rules[0].Quant.has_value());
  EXPECT_EQ(Rule::Quantifier::Exists, *P.Rules[0].Quant);
}

TEST(SmplParser, HeaderOptionOrderIsFixed) {
  EXPECT_NE(std::string::npos,
            rejection("@r1@\n@@\n* foo();\n\n@r2 exists depends on r1@\n@@\n"
                      "* bar();\n")
                .find("must come before"));
}

TEST(SmplParser, DependencyMustBeDeclaredEarlier) {
  EXPECT_NE(std::string::npos,
            rejection("@r depends on nosuchrule@\n@@\n* foo();\n")
                .find("nosuchrule"));
  // A forward reference is a parse error in Coccinelle, not a late binding.
  EXPECT_NE(std::string::npos,
            rejection("@r1 depends on r2@\n@@\n* foo();\n\n@r2@\n@@\n"
                      "* bar();\n")
                .find("r2"));
}

TEST(SmplParser, DuplicateRuleName) {
  EXPECT_NE(std::string::npos,
            rejection("@r@\n@@\n* foo();\n\n@r@\n@@\n* bar();\n")
                .find("duplicate rule name"));
}

TEST(SmplParser, IndentedHeaderWithOptionsIsRejected) {
  // '@@' is one lexer token and may be indented; a header carrying options
  // may not.
  EXPECT_NE(std::string::npos,
            rejection("@r1@\n@@\n* foo();\n\n   @r2@\n@@\n* bar();\n")
                .find("column 0"));
  EXPECT_TRUE(parsed("   @@\n@@\n* foo();\n").fullyUnderstood());
}

TEST(SmplParser, VirtualRulesAndModeSelection) {
  // Every kernel patch selects its output mode this way, so it is in the
  // subset: the virtuals are recorded and the dependency is marked virtual.
  SemanticPatch P = parsed("virtual patch\nvirtual report\n\n"
                           "@r depends on report@\nexpression E;\n@@\n"
                           "* foo(E);\n");
  ASSERT_EQ(2u, P.Virtuals.size());
  EXPECT_EQ("patch", P.Virtuals[0]);
  EXPECT_EQ("report", P.Virtuals[1]);
  ASSERT_EQ(1u, P.Rules.size());
  EXPECT_EQ("report", P.Rules[0].DependsOn);
  EXPECT_TRUE(P.Rules[0].DependsOnVirtual);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  // A boolean over virtuals alone is still mode selection.
  SemanticPatch Q = parsed("virtual patch\nvirtual report\n\n"
                           "@r depends on report && !patch@\n@@\n* foo();\n");
  EXPECT_TRUE(Q.Rules[0].DependsOnVirtual);
  EXPECT_TRUE(Q.fullyUnderstood()) << refusalList(Q);
}

TEST(SmplParser, DependencyMixingARuleNameIntoABooleanIsRefused) {
  // Mixing a rule name in makes the dependency a per-match property, which
  // is the suppression idiom misc/struct_size.cocci is built on.
  SemanticPatch P = parsed("virtual report\n\n@r1@\n@@\n* foo();\n\n"
                           "@r2 depends on !r1 && report@\n@@\n* bar();\n");
  EXPECT_REFUSED(P, "depends on negated rule");
  EXPECT_REFUSED(P, "depends on boolean expression over a rule name");
  EXPECT_FALSE(P.Rules[1].DependsOnVirtual);
  expectWellFormedRefusals(P);
}

TEST(SmplParser, SixMetaVarKinds) {
  SemanticPatch P = parsed("@r@\nexpression E;\nidentifier f;\nstatement S;\n"
                           "type T;\nconstant C;\nposition p;\n@@\n"
                           "* f@p((T)E, C);\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(6u, P.Rules[0].MetaVars.size());
  EXPECT_EQ(MetaKind::Expression, P.Rules[0].MetaVars[0].Kind);
  EXPECT_EQ(MetaKind::Identifier, P.Rules[0].MetaVars[1].Kind);
  EXPECT_EQ(MetaKind::Statement, P.Rules[0].MetaVars[2].Kind);
  EXPECT_EQ(MetaKind::Type, P.Rules[0].MetaVars[3].Kind);
  EXPECT_EQ(MetaKind::Constant, P.Rules[0].MetaVars[4].Kind);
  EXPECT_EQ(MetaKind::Position, P.Rules[0].MetaVars[5].Kind);
  EXPECT_EQ("p", P.Rules[0].Body[0].PositionVar);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
}

TEST(SmplParser, CommaSeparatedDeclarationAndEmptyBlock) {
  SemanticPatch P = parsed("@r@\nexpression x, E;\n@@\n* foo(x, E);\n");
  EXPECT_EQ(2u, P.Rules[0].MetaVars.size());
  SemanticPatch Q = parsed("@r@\n@@\n* foo();\n");
  EXPECT_TRUE(Q.Rules[0].MetaVars.empty());
}

TEST(SmplParser, InheritedMetaVar) {
  SemanticPatch P = parsed("@r1@\nexpression E;\n@@\n foo(E);\n\n"
                           "@r2@\nexpression r1.E;\n@@\n* bar(E);\n");
  ASSERT_EQ(2u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[1].MetaVars.size());
  EXPECT_EQ("r1", P.Rules[1].MetaVars[0].InheritedFrom);
  EXPECT_EQ("E", P.Rules[1].MetaVars[0].Name);
  // A kind mismatch and an unknown variable are both errors.
  EXPECT_NE(std::string::npos,
            rejection("@r1@\nexpression E;\n@@\n foo(E);\n\n"
                      "@r2@\nposition r1.E;\n@@\n* bar(E);\n")
                .find("incompatible inheritance"));
  EXPECT_NE(std::string::npos,
            rejection("@r1@\nexpression E;\n@@\n foo(E);\n\n"
                      "@r2@\nexpression r1.Q;\n@@\n* bar(Q);\n")
                .find("declares no metavariable"));
}

TEST(SmplParser, PerNameConstraintsInOneDeclaration) {
  // `position free.p1!=loop.ok,p2!={print.p,sz.p};` declares two positions,
  // each with its own constraint, and the set braces hold commas of their own.
  SemanticPatch P =
      parsed("@loop@\nposition ok;\n@@\n while@ok (1) {}\n\n"
             "@r@\nposition loop.ok, p2 != {loop.ok};\n@@\n* foo@p2();\n");
  ASSERT_EQ(2u, P.Rules.size());
  ASSERT_EQ(2u, P.Rules[1].MetaVars.size());
  EXPECT_EQ("ok", P.Rules[1].MetaVars[0].Name);
  EXPECT_EQ("p2", P.Rules[1].MetaVars[1].Name);
  EXPECT_REFUSED(P, "set-valued metavariable constraint");
}

TEST(SmplParser, RedeclaredMetaVarAndArityPrefix) {
  EXPECT_NE(std::string::npos,
            rejection("@r@\nexpression E;\nexpression E;\n@@\n* foo(E);\n")
                .find("declared twice"));
  EXPECT_NE(
      std::string::npos,
      rejection("@r@\n?expression E;\n@@\n* foo(E);\n").find("arity prefix"));
}

TEST(SmplParser, PositionInheritanceOverAModification) {
  // Coccinelle clears its inheritable-position list as soon as a rule
  // modifies the tree, and it clears rather than filters, so an unrelated
  // modifying rule in between breaks the inheritance too.
  EXPECT_NE(std::string::npos,
            rejection("@r1@\nposition p;\n@@\n foo@p();\n\n"
                      "@r2@\n@@\n- bar();\n\n"
                      "@r3@\nposition r1.p;\n@@\n baz@p();\n")
                .find("cannot be inherited"));
}

TEST(SmplParser, Markers) {
  SemanticPatch P = parsed("@r@\n@@\n  ctx();\n- gone();\n+ added();\n");
  ASSERT_EQ(3u, P.Rules[0].Body.size());
  EXPECT_EQ(ItemMarker::Context, P.Rules[0].Body[0].Marker);
  EXPECT_EQ(ItemMarker::Minus, P.Rules[0].Body[1].Marker);
  EXPECT_EQ(ItemMarker::Plus, P.Rules[0].Body[2].Marker);
  SemanticPatch Q = parsed("@r@\n@@\n* starred();\n");
  EXPECT_EQ(ItemMarker::Star, Q.Rules[0].Body[0].Marker);
}

TEST(SmplParser, IndentedMarkerIsRefused) {
  // The trap: indented, '-' is unary minus and '*' is a dereference, so
  // Coccinelle reads the line as context and transforms nothing, silently.
  SemanticPatch P = parsed("@r@\nexpression E;\n@@\n  ctx();\n  - foo(E);\n");
  EXPECT_REFUSED(P, "indented line marker");
  for (const PatternItem &It : P.Rules[0].Body)
    EXPECT_EQ(ItemMarker::Context, It.Marker);
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n  ctx();\n  * foo(E);\n"),
                 "indented line marker");
}

TEST(SmplParser, PatchAndMatchModeCannotMix) {
  EXPECT_NE(std::string::npos,
            rejection("@r1@\n@@\n* foo();\n\n@r2@\n@@\n- bar();\n")
                .find("never both"));
  // Unless the two rules are gated on different virtuals, which is how every
  // kernel patch ships a '*' variant and a '-'/'+' variant in one file.
  SemanticPatch P = parsed("virtual patch\nvirtual report\n\n"
                           "@r1 depends on report@\n@@\n* foo();\n\n"
                           "@r2 depends on patch@\n@@\n- bar();\n");
  EXPECT_EQ(2u, P.Rules.size());
}

TEST(SmplParser, PlusSliceNeedsAMinusOrContextLine) {
  EXPECT_NE(
      std::string::npos,
      rejection("@r@\n@@\n+ foo();\n").find("minus slice can't be empty"));
}

TEST(SmplParser, StatementDotsAndWhenNot) {
  SemanticPatch P = parsed("@r exists@\nexpression x, E;\n@@\n  foo(x);\n"
                           "  ... when != x = E\n- bar(x);\n");
  ASSERT_EQ(3u, P.Rules[0].Body.size());
  EXPECT_EQ(ItemKind::Dots, P.Rules[0].Body[1].Kind);
  ASSERT_EQ(1u, P.Rules[0].Body[1].WhenNot.size());
  EXPECT_EQ("x = E", P.Rules[0].Body[1].WhenNot[0]);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
}

TEST(SmplParser, DotsAfterOtherCodeOnTheSameLine) {
  // `if (E) { ... when != f(e)` is one statement plus a statement-level
  // ellipsis carrying the clause.
  SemanticPatch P = parsed("@r exists@\nexpression E, e;\n@@\n"
                           "  if (E) { ... when != f(e)\n  }\n- bar(e);\n");
  ASSERT_LE(3u, P.Rules[0].Body.size());
  EXPECT_EQ(ItemKind::Statement, P.Rules[0].Body[0].Kind);
  EXPECT_EQ("if (E) {", P.Rules[0].Body[0].Text);
  EXPECT_EQ(ItemKind::Dots, P.Rules[0].Body[1].Kind);
  ASSERT_EQ(1u, P.Rules[0].Body[1].WhenNot.size());
  EXPECT_EQ("f(e)", P.Rules[0].Body[1].WhenNot[0]);
}

TEST(SmplParser, TwoWhenClausesOnSeparateLinesBothAttach) {
  SemanticPatch P = parsed("@r exists@\nexpression E;\n@@\n  foo(E);\n"
                           "  ... when != baz(E)\n      when != qux(E)\n"
                           "- bar(E);\n");
  ASSERT_EQ(3u, P.Rules[0].Body.size());
  ASSERT_EQ(2u, P.Rules[0].Body[1].WhenNot.size());
  EXPECT_EQ("baz(E)", P.Rules[0].Body[1].WhenNot[0]);
  EXPECT_EQ("qux(E)", P.Rules[0].Body[1].WhenNot[1]);
}

TEST(SmplParser, TwoWhenClausesOnOneLineIsAnError) {
  // Each 'when' runs to the end of its physical line, so the second is
  // swallowed as part of the first and Coccinelle has no production for it.
  EXPECT_NE(std::string::npos,
            rejection("@r exists@\nexpression E;\n@@\n  foo(E);\n"
                      "  ... when != baz(E) when != qux(E)\n- bar(E);\n")
                .find("two 'when' clauses on one line"));
}

TEST(SmplParser, BackslashContinuationSplicesTheWhenLine) {
  // One clause continued over a '\' is accepted by Coccinelle and refused
  // here; two clauses split over one is a parse error there and here.
  SemanticPatch P = parsed("@r exists@\nexpression E;\n@@\n  foo(E);\n"
                           "  ... when != baz(E) + \\\n      qux(E)\n"
                           "- bar(E);\n");
  EXPECT_REFUSED(P, "backslash line continuation");
  EXPECT_NE(std::string::npos,
            rejection("@r exists@\nexpression E;\n@@\n  foo(E);\n"
                      "  ... when != baz(E) \\\n      when != qux(E)\n"
                      "- bar(E);\n")
                .find("two 'when' clauses on one line"));
}

TEST(SmplParser, ClosingBraceOnTheWhenLineIsAnError) {
  EXPECT_NE(std::string::npos,
            rejection("@r exists@\nexpression E;\n@@\n  if (E) { ... when != "
                      "baz(E) }\n- bar(E);\n")
                .find("closing brace"));
}

TEST(SmplParser, WhenModifiers) {
  SemanticPatch P = parsed("@r exists@\n@@\n  foo();\n  ... when any\n"
                           "- bar();\n");
  EXPECT_TRUE(P.Rules[0].Body[1].WhenAny);
  EXPECT_FALSE(P.Rules[0].Body[1].WhenStrict);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  // Modifiers use commas, not repetition, and both have a field.
  SemanticPatch Q = parsed("@r exists@\n@@\n  foo();\n  ... when any, strict\n"
                           "- bar();\n");
  EXPECT_TRUE(Q.Rules[0].Body[1].WhenAny);
  EXPECT_TRUE(Q.Rules[0].Body[1].WhenStrict);
  EXPECT_TRUE(Q.fullyUnderstood()) << refusalList(Q);
  // The per-ellipsis quantifiers override the rule's own, which the
  // representation cannot express.
  EXPECT_REFUSED(parsed("@r exists@\n@@\n  foo();\n  ... when forall\n"
                        "- bar();\n"),
                 "when forall");
  EXPECT_REFUSED(parsed("@r exists@\n@@\n  foo();\n  ... when exists\n"
                        "- bar();\n"),
                 "when exists");
  EXPECT_REFUSED(parsed("@r exists@\nexpression E;\n@@\n  foo();\n"
                        "  ... when = bar(E);\n- baz();\n"),
                 "when = <statement>");
  EXPECT_REFUSED(parsed("@r exists@\nexpression E;\n@@\n  foo();\n"
                        "  ... when != true E\n- baz();\n"),
                 "when != true / when != false");
  EXPECT_REFUSED(parsed("@r exists@\nexpression E;\n@@\n  foo();\n"
                        "  ... when == bar(E);\n- baz();\n"),
                 "when == code");
}

TEST(SmplParser, WhenNeqCannotCarryAModifier) {
  EXPECT_NE(std::string::npos,
            rejection("@r exists@\nexpression E;\n@@\n  foo(E);\n"
                      "  ... when != baz(E), any\n- bar(E);\n")
                .find("cannot be combined"));
}

TEST(SmplParser, BareExistsAfterDotsIsAnError) {
  // Coccinelle reads the bare keyword as a typedef name and then fails
  // somewhere else. The per-ellipsis form is 'when exists'.
  EXPECT_NE(std::string::npos,
            rejection("@r@\nexpression E;\n@@\n  foo(E);\n  ... exists\n"
                      "- bar(E);\n")
                .find("not \x53mPL"));
}

TEST(SmplParser, ColumnZeroDisjunction) {
  SemanticPatch P = parsed("@r@\nexpression E;\n@@\n(\n* foo(E);\n|\n"
                           "* bar(E);\n)\n");
  ASSERT_EQ(1u, P.Rules[0].Body.size());
  const PatternItem &D = P.Rules[0].Body[0];
  EXPECT_EQ(ItemKind::Disjunction, D.Kind);
  ASSERT_EQ(2u, D.Branches.size());
  ASSERT_EQ(1u, D.Branches[0].size());
  EXPECT_EQ("foo(E);", D.Branches[0][0].Text);
  EXPECT_EQ("bar(E);", D.Branches[1][0].Text);
  EXPECT_EQ(ItemMarker::Star, D.Branches[0][0].Marker);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
}

TEST(SmplParser, ADeclaredTypeNameIsRecordedForTheWholePatch) {
  // Coccinelle's type table is per file, and `tests/wchar.cocci` relies on
  // it: the names are declared in the first rule's header and written in the
  // second rule's body.
  SemanticPatch P =
      parsed("@r@\ntypedef char16_t, wchar_t;\n@@\n- char16_t a = u'x';\n"
             "\n@@\nidentifier b;\n@@\n- wchar_t b = L'x';\n");
  ASSERT_EQ(2u, P.TypeNames.size());
  EXPECT_EQ("char16_t", P.TypeNames[0]);
  EXPECT_EQ("wchar_t", P.TypeNames[1]);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
}

TEST(SmplParser, AKeywordCannotBeDeclaredAsATypeName) {
  // `typedef int;` used to reach the synthesised pattern source as
  // `typedef int int;`, which does not compile, and the diagnostic sits
  // outside every item's wrapper so nothing reported it: the pattern still
  // came back as a node and the rewrite was applied. Coccinelle rejects the
  // declaration at meta-parse.
  EXPECT_NE(std::string::npos,
            rejection("@a@\ntypedef int;\n@@\n- int v = 0;\n+ long v = 0;\n")
                .find("'int' is a C keyword"));
  EXPECT_NE(std::string::npos,
            rejection("@a@\ntypedef struct;\n@@\n- int v;\n+ long v;\n")
                .find("is a C keyword"));
}

TEST(SmplParser, EachBranchOfADisjunctionIsGroupedOncePerSide) {
  // A branch is a rule body in miniature, so it interleaves its `-`, `+` and
  // context lines the way a rule does and has to be grouped per side for the
  // same reason. Left ungrouped, a branch held one raw line per source line
  // and no consumer of a grouped side could read it.
  SemanticPatch P = parsed("@r@\nexpression E;\n@@\n(\n  if (\n- E == 0\n"
                           "+ !E\n  )\n  { }\n|\n- foo(E);\n+ bar(E);\n)\n");
  ASSERT_EQ(1u, P.Rules[0].Minus.size());
  ASSERT_EQ(1u, P.Rules[0].Plus.size());
  const PatternItem &Minus = P.Rules[0].Minus[0];
  const PatternItem &Plus = P.Rules[0].Plus[0];
  ASSERT_EQ(ItemKind::Disjunction, Minus.Kind);
  ASSERT_EQ(2u, Minus.Branches.size());
  ASSERT_EQ(2u, Plus.Branches.size());
  ASSERT_EQ(1u, Minus.Branches[0].size());
  EXPECT_EQ("if ( E == 0 ) { }", Minus.Branches[0][0].Text);
  EXPECT_EQ("if ( !E ) { }", Plus.Branches[0][0].Text);
  EXPECT_EQ("foo(E);", Minus.Branches[1][0].Text);
  EXPECT_EQ("bar(E);", Plus.Branches[1][0].Text);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
}

/// The substring of \p It's text that \p Sp names.
StringRef spanText(const PatternItem &It, const PatternItem::Span &Sp) {
  return StringRef(It.Text).substr(Sp.Offset, Sp.Length);
}

TEST(SmplParser, AGroupedStatementSaysWhichPartEachPatchLineWrote) {
  SemanticPatch P = parsed("@@\nidentifier i, i2;\nstatement S;\nconstant c;"
                           "\n@@\n  if(\n- (i = i2)\n+ i\n  +\n  c) S\n");
  ASSERT_EQ(1u, P.Rules[0].Minus.size());
  const PatternItem &Minus = P.Rules[0].Minus[0];
  EXPECT_EQ("if( (i = i2) + c) S", Minus.Text);
  ASSERT_EQ(4u, Minus.Spans.size());
  EXPECT_EQ("if(", spanText(Minus, Minus.Spans[0]));
  EXPECT_EQ(ItemMarker::Context, Minus.Spans[0].LineMarker);
  EXPECT_EQ("(i = i2)", spanText(Minus, Minus.Spans[1]));
  EXPECT_EQ(ItemMarker::Minus, Minus.Spans[1].LineMarker);
  EXPECT_EQ("+", spanText(Minus, Minus.Spans[2]));
  EXPECT_EQ("c) S", spanText(Minus, Minus.Spans[3]));

  ASSERT_EQ(1u, P.Rules[0].Plus.size());
  const PatternItem &Plus = P.Rules[0].Plus[0];
  EXPECT_EQ("if( i + c) S", Plus.Text);
  ASSERT_EQ(4u, Plus.Spans.size());
  EXPECT_EQ("i", spanText(Plus, Plus.Spans[1]));
  EXPECT_EQ(ItemMarker::Plus, Plus.Spans[1].LineMarker);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
}

TEST(SmplParser, AnUngroupedLineIsOneSpanOverTheWholeOfItsText) {
  SemanticPatch P = parsed("@@\nexpression E;\n@@\n- foo(E);\n+ bar(E);\n");
  const PatternItem &Minus = P.Rules[0].Minus[0];
  ASSERT_EQ(1u, Minus.Spans.size());
  EXPECT_EQ("foo(E);", spanText(Minus, Minus.Spans[0]));
  EXPECT_EQ(ItemMarker::Minus, Minus.Spans[0].LineMarker);
}

TEST(SmplParser, PairingSplitsAStatementTheWayALineDiffSplitsOne) {
  SemanticPatch P = parsed("@@\nidentifier i, i2;\nstatement S;\nconstant c;"
                           "\n@@\n  if(\n- (i = i2)\n+ i\n  +\n  c) S\n");
  unsigned Insertions = 0;
  std::vector<PatternHunk> H =
      pairHunks(P.Rules[0].Minus[0], P.Rules[0].Plus[0], Insertions);
  ASSERT_EQ(1u, H.size());
  EXPECT_EQ(0u, Insertions);
  EXPECT_EQ("(i = i2)", StringRef(P.Rules[0].Minus[0].Text)
                            .substr(H[0].MinusOffset, H[0].MinusLength));
  EXPECT_EQ("i", H[0].PlusText);
}

TEST(SmplParser, PairingCountsAPlusRunThatNoMinusRunPrecedes) {
  // `binop` writes a whole statement on a `+` line before the `if` it also
  // rewrites. That is an insertion placed relative to the match, which is a
  // different step, so pairing counts it rather than dropping the line.
  SemanticPatch P = parsed("@@\nidentifier i, i2;\nstatement S;\nconstant c;"
                           "\n@@\n+ i = i2;\n  if(\n- (i = i2)\n+ i\n"
                           "  +\n  c) S\n");
  ASSERT_EQ(2u, P.Rules[0].Plus.size());
  unsigned Insertions = 0;
  std::vector<PatternHunk> H =
      pairHunks(P.Rules[0].Minus[0], P.Rules[0].Plus[1], Insertions);
  ASSERT_EQ(1u, H.size());
  EXPECT_EQ("i", H[0].PlusText);
  unsigned Alone = 0;
  pairHunks(PatternItem(), P.Rules[0].Plus[0], Alone);
  EXPECT_EQ(1u, Alone);
}

TEST(SmplParser, PairingReadsADeletionWithNoPlusRunAsADeletion) {
  SemanticPatch P = parsed("@@\nexpression E;\n@@\n  foo(\n- E\n  );\n");
  unsigned Insertions = 0;
  std::vector<PatternHunk> H =
      pairHunks(P.Rules[0].Minus[0], P.Rules[0].Plus[0], Insertions);
  ASSERT_EQ(1u, H.size());
  EXPECT_EQ("E", StringRef(P.Rules[0].Minus[0].Text)
                     .substr(H[0].MinusOffset, H[0].MinusLength));
  EXPECT_TRUE(H[0].PlusText.empty());
}

TEST(SmplParser, CollectingASideDescendsIntoEveryBranch) {
  // A rule whose whole body is a disjunction has all its pattern statements
  // inside branches, so a reader that stops at the top level reports the rule
  // as having none and hides it from any sweep over that output.
  SemanticPatch P = parsed("@r@\nexpression E;\n@@\n"
                           "(\n- foo(E);\n+ a(E);\n|\n- bar(E);\n+ b(E);\n)\n");
  std::vector<std::string> Minus;
  collectStatements(P.Rules[0].Minus, Minus);
  ASSERT_EQ(2u, Minus.size());
  EXPECT_EQ("foo(E);", Minus[0]);
  EXPECT_EQ("bar(E);", Minus[1]);
  std::vector<std::string> Plus;
  collectStatements(P.Rules[0].Plus, Plus);
  ASSERT_EQ(2u, Plus.size());
  EXPECT_EQ("a(E);", Plus[0]);
  EXPECT_EQ("b(E);", Plus[1]);
}

TEST(SmplParser, IndentedDisjunctionDelimiter) {
  // Indented, '|' is a bitwise or, so the alternation meant here is not
  // applied. An indented '(' or ')' is an ordinary parenthesis, which is
  // what a statement continued over lines uses, so that is not flagged.
  SemanticPatch P = parsed("@r@\nexpression E;\n@@\n(\n* foo(E);\n  |\n"
                           "* bar(E);\n)\n");
  EXPECT_REFUSED(P, "indented disjunction delimiter");
  SemanticPatch Q = parsed("@r@\nexpression E;\n@@\n  if (\n- E == 0\n"
                           "+ !E\n  )\n  { }\n");
  EXPECT_NOT_REFUSED(Q, "indented disjunction delimiter");
}

TEST(SmplParser, PositionAttachment) {
  SemanticPatch P =
      parsed("@r@\nexpression E;\nposition p;\n@@\n* foo@p(E);\n");
  EXPECT_EQ("p", P.Rules[0].Body[0].PositionVar);
  EXPECT_EQ("foo(E);", P.Rules[0].Body[0].Text);
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  // A keyword is a token like any other.
  SemanticPatch Q = parsed("@r@\nexpression E;\nposition p;\n@@\n"
                           "* if@p (E) { }\n");
  EXPECT_EQ("p", Q.Rules[0].Body[0].PositionVar);
}

TEST(SmplParser, PositionAttachmentTraps) {
  // Undeclared, a space after the '@', on a '+' line, on a disjunction
  // delimiter, and first on a line: all rejected outright.
  EXPECT_NE(
      std::string::npos,
      rejection("@r@\nexpression E;\n@@\n* foo@p(E);\n").find("not declared"));
  EXPECT_NE(std::string::npos,
            rejection("@r@\nexpression E;\nposition p;\n@@\n* foo@ p(E);\n")
                .find("whitespace between"));
  EXPECT_NE(std::string::npos,
            rejection("@r@\nexpression E;\nposition p;\n@@\n- foo(E);\n"
                      "+ baz@p(E);\n")
                .find("not allowed on a '+' line"));
  EXPECT_NE(std::string::npos,
            rejection("@r@\nexpression E;\nposition p;\n@@\n(\n* foo(E);\n"
                      "|@p\n* bar(E);\n)\n")
                .find("disjunction delimiter"));
  EXPECT_NE(std::string::npos,
            rejection("@r@\nexpression E;\nposition p;\n@@\n* foo\n@p(E);\n")
                .find("unterminated rule header"));
  // Two on one line: PatternItem holds one, so the second is refused.
  SemanticPatch P = parsed("@r@\nexpression E;\nposition p1, p2;\n@@\n"
                           "* foo@p1(E@p2);\n");
  EXPECT_EQ("p1", P.Rules[0].Body[0].PositionVar);
  EXPECT_REFUSED(P, "more than one position attachment on one line");
  // Whitespace before the '@' is accepted by Coccinelle and refused here so
  // the two spellings of one attachment cannot diverge.
  EXPECT_REFUSED(parsed("@r@\nexpression E;\nposition p;\n@@\n* foo @p (E);\n"),
                 "whitespace between token and @");
  EXPECT_REFUSED(parsed("@r exists@\nexpression E;\nposition p;\n@@\n"
                        "  foo(E);\n  ...@p\n- bar(E);\n"),
                 "position attached to an ellipsis, a delimiter or a "
                 "separator");
  EXPECT_REFUSED(parsed("@r@\nexpression E1, E2;\nposition p;\n@@\n"
                        "* E1 +@p E2\n"),
                 "position attached to an operator");
}

TEST(SmplParser, ScriptRules) {
  // A Python rule that formats and prints is inside the subset: it reports,
  // it does not change what the pattern matches.
  SemanticPatch P =
      parsed("virtual report\n\n"
             "@r@\nexpression E;\nposition p;\n@@\n* foo@p(E);\n\n"
             "@script:python depends on report@\np << r.p;\n@@\n"
             "msg = \"found\"\n"
             "coccilib.report.print_report(p[0], msg)\n");
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  EXPECT_EQ(1u, P.Rules.size());
  // Anything else in the body is named.
  EXPECT_REFUSED(parsed("@r@\nexpression E;\nposition p;\n@@\n* foo@p(E);\n\n"
                        "@script:python@\np << r.p;\n@@\n"
                        "import sys\n"),
                 "script:python body beyond print_report/print_todo/print");
  EXPECT_REFUSED(parsed("@r@\nexpression E;\nposition p;\n@@\n* foo@p(E);\n\n"
                        "@script:python@\np << r.p;\n@@\n"
                        "cocci.print_main(\"x\", p)\n"),
                 "coccilib call other than print_report/print_todo");
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n* foo(E);\n\n"
                        "@script:ocaml@\n@@\nlet x = 1\n"),
                 "script:ocaml rule");
  EXPECT_REFUSED(parsed("@initialize:python@\n@@\nimport sys\n\n"
                        "@r@\nexpression E;\n@@\n* foo(E);\n"),
                 "@initialize:python@ / @finalize:python@ rule");
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n* foo(E);\n\n"
                        "@finalize:ocaml@\n@@\nlet y = 2\n"),
                 "@initialize:ocaml@ / @finalize:ocaml@ rule");
  // A bare '@' in a script body ends the body for Coccinelle, so the file it
  // reads is not the file that was written.
  EXPECT_NE(std::string::npos,
            rejection("@r@\nexpression E;\nposition p;\n@@\n* foo@p(E);\n\n"
                      "@script:python@\np << r.p;\n@@\nx = 1 @ 2\n")
                .find("bare '@'"));
  // Bindings outside `x << r.y` are named one by one.
  EXPECT_REFUSED(parsed("@r@\nidentifier f;\n@@\n* f();\n\n"
                        "@script:python@\n(str, ast) << r.f;\n@@\n"
                        "print(str)\n"),
                 "script (str, ast) binding");
  EXPECT_REFUSED(parsed("@r@\nidentifier f;\n@@\n* f();\n\n"
                        "@script:python@\nf << r.f = \"d\";\n@@\n"
                        "print(f)\n"),
                 "script binding with a default value");
  EXPECT_REFUSED(parsed("@r@\nidentifier f;\n@@\n* f();\n\n"
                        "@script:python@\nx;\n@@\nprint(1)\n"),
                 "script output variable (declared without <<)");
}

TEST(SmplParser, HeaderOptionsOutsideTheSubset) {
  EXPECT_REFUSED(parsed("@r1@\nexpression E;\n@@\n foo(E);\n\n"
                        "@r2 extends r1@\n@@\n* bar(E);\n"),
                 "extends");
  EXPECT_REFUSED(parsed("@r using \"my.iso\"@\n@@\n* foo();\n"),
                 "using \"...\" isomorphism file");
  EXPECT_REFUSED(parsed("@r disable optional_qualifier@\n@@\n* foo();\n"),
                 "disable <isomorphism>");
  EXPECT_REFUSED(parsed("@r depends on file in \"t.c\"@\n@@\n* foo();\n"),
                 "depends on file in");
  EXPECT_REFUSED(parsed("@r1@\n@@\n foo();\n\n@r2 depends on ever r1@\n@@\n"
                        "* bar();\n"),
                 "depends on ever / never");
  EXPECT_REFUSED(parsed("#spatch --c++\n\n@@\n@@\n* foo();\n"),
                 "#spatch --c++");
}

TEST(SmplParser, InertSpatchOptionsAreAcceptedAndTheRestAreNot) {
  // An embedded option line is configuration rather than pattern semantics,
  // but only while the options cannot change how a pattern is read.
  EXPECT_TRUE(parsed("#spatch --very-quiet --timeout 60\n\n@@\n@@\n"
                     "* foo();\n")
                  .fullyUnderstood());
  EXPECT_TRUE(parsed("# spatch --smpl-spacing\n\n@@\n@@\n* foo();\n")
                  .fullyUnderstood());
  // The C++ front end reads every pattern in the file differently, so
  // accepting the line and ignoring it would be a silent misparse.
  EXPECT_REFUSED(parsed("#spatch --c++=11\n\n@@\n@@\n* foo();\n"),
                 "#spatch --c++");
  // Three that reach past the output: the control-flow graph, the integer
  // model, and which rules are live.
  EXPECT_REFUSED(parsed("#spatch --no-gotos\n\n@@\n@@\n* foo();\n"),
                 "#spatch embedded options");
  EXPECT_REFUSED(parsed("#spatch --int-bits 32\n\n@@\n@@\n* foo();\n"),
                 "#spatch embedded options");
  EXPECT_REFUSED(parsed("virtual report\n#spatch -D report\n\n@@\n@@\n"
                        "* foo();\n"),
                 "#spatch embedded options");
  // An unknown option cannot be judged, so it is refused as well.
  EXPECT_REFUSED(parsed("#spatch --fake-option\n\n@@\n@@\n* foo();\n"),
                 "#spatch embedded options");
}

TEST(SmplParser, SymbolDeclarationIsAccepted) {
  // `symbol x;` says the name is literal C text rather than a metavariable,
  // which is already what an undeclared name means here, so nothing is
  // recorded and nothing is lost.
  SemanticPatch P = parsed("@r@\nsymbol NULL;\nexpression E;\n@@\n"
                           "* foo(E, NULL);\n");
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  ASSERT_EQ(1u, P.Rules[0].MetaVars.size());
  EXPECT_EQ("E", P.Rules[0].MetaVars[0].Name);
  EXPECT_EQ("foo(E, NULL);", P.Rules[0].Body[0].Text);
  // It also says the author meant the name literally, so the reuse trap
  // does not fire on a name another rule declares as a metavariable.
  SemanticPatch Q = parsed("@r1@\nexpression E;\n@@\n foo(E);\n\n"
                           "@r2@\nsymbol E;\n@@\n* bar(E);\n");
  EXPECT_TRUE(Q.fullyUnderstood()) << refusalList(Q);
  EXPECT_NOT_REFUSED(Q, "metavariable name used as a plain C identifier");
  // A constraint on one makes it more than a statement about the name.
  EXPECT_REFUSED(parsed("@r@\nsymbol f = {a, b};\n@@\n* f();\n"),
                 "symbol declaration");
}

TEST(SmplParser, TypedefDeclarationIsAccepted) {
  // `typedef t;` tells Coccinelle's own C parser that the name is a type
  // name. Clang takes that from the translation unit, so the declaration
  // changes nothing in the compiled pattern.
  SemanticPatch P = parsed("@r@\ntypedef Scsi_Cmnd;\nexpression E;\n@@\n"
                           "* foo((Scsi_Cmnd *)E);\n");
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  EXPECT_EQ(1u, P.Rules[0].MetaVars.size());
  // The one-line declaration block form, and the comma list.
  EXPECT_TRUE(
      parsed("@@ typedef dev_link_t; @@\n* foo();\n").fullyUnderstood());
  EXPECT_TRUE(
      parsed("@r@\ntypedef ty_id, ty_id2;\n@@\n* foo();\n").fullyUnderstood());
}

TEST(SmplParser, OneLineBlockBecomesItemsRatherThanText) {
  // `{ ... }` on one line is a block holding the statement-level ellipsis.
  // It is split into items, because a '...' left in a statement's text would
  // reach the compiler as C.
  SemanticPatch P = parsed("@r exists@\nidentifier f;\n@@\n  f (void)\n"
                           "  { ... }\n");
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  ASSERT_EQ(4u, P.Rules[0].Body.size());
  EXPECT_EQ("f (void)", P.Rules[0].Body[0].Text);
  EXPECT_EQ("{", P.Rules[0].Body[1].Text);
  EXPECT_EQ(ItemKind::Dots, P.Rules[0].Body[2].Kind);
  EXPECT_EQ("}", P.Rules[0].Body[3].Text);
  for (const PatternItem &It : P.Rules[0].Body)
    EXPECT_EQ(std::string::npos, It.Text.find("..."))
        << "an ellipsis reached a statement's text: " << It.Text;
  // A brace that follows a name opens a record body, so the ellipsis in it
  // is a field list and stays refused.
  EXPECT_REFUSED(parsed("@r@\ntype T1, T2;\n@@\n- typedef T1 { ... } T2;\n"),
                 "field-level ellipsis");
  // The block of a function definition is split too, and what is left is
  // the definition pattern itself, which needs a declaration matcher.
  EXPECT_REFUSED(parsed("@r@\n@@\n- main() {...}\n"),
                 "function definition pattern");
}

TEST(SmplParser, MetaVarKindsOutsideTheSubset) {
  EXPECT_REFUSED(parsed("@r@\nexpression list el;\n@@\n* foo(el);\n"),
                 "expression list metavariable");
  EXPECT_REFUSED(parsed("@r@\nmetavariable m;\n@@\n* foo(m);\n"),
                 "metavariable (any kind)");
  EXPECT_REFUSED(parsed("@r@\nlocal idexpression x;\n@@\n* foo(x);\n"),
                 "local idexpression metavariable");
  EXPECT_REFUSED(parsed("@r@\niterator I;\nstatement S;\n@@\n* I(1) S\n"),
                 "iterator metavariable");
  EXPECT_REFUSED(parsed("@r@\ndeclarer name DECL;\n@@\n* DECL(x);\n"),
                 "declarer name declaration");
  EXPECT_REFUSED(parsed("@r@\nbinary operator bop;\nexpression E1, E2;\n@@\n"
                        "* E1 bop E2\n"),
                 "binary operator metavariable");
  EXPECT_REFUSED(parsed("@r@\ncomments c;\nexpression E;\n@@\n* foo(E);\n"),
                 "comments metavariable");
  EXPECT_REFUSED(parsed("@r@\nposition any p;\n@@\n* foo@p();\n"),
                 "position any");
  EXPECT_REFUSED(parsed("@r@\nidentifier f =~ \"^foo\";\n@@\n* f();\n"),
                 "regex constraint on a metavariable");
  EXPECT_REFUSED(parsed("@r@\nidentifier f != {a, b};\n@@\n* f();\n"),
                 "set-valued metavariable constraint");
  EXPECT_REFUSED(parsed("@r@\nidentifier f = foo;\n@@\n* f();\n"),
                 "value constraint on a metavariable");
  EXPECT_REFUSED(parsed("@r@\nexpression E : struct foo *;\n@@\n* bar(E);\n"),
                 "type-restricted metavariable declaration");
  EXPECT_REFUSED(parsed("@r@\nidentifier f : script:python() { True };\n@@\n"
                        "* f();\n"),
                 "python constraint on a metavariable");
  EXPECT_REFUSED(parsed("@r@\ntype T;\nT [] x;\n@@\n* foo(x);\n"),
                 "array-typed metavariable declaration");
  SemanticPatch Fresh =
      parsed("@r@\nidentifier f;\nfresh identifier g = f ## \"_new\";\n@@\n"
             "- f();\n+ g();\n");
  EXPECT_REFUSED(Fresh, "fresh identifier");
  EXPECT_REFUSED(Fresh, "fresh identifier with ## concatenation");
}

TEST(SmplParser, EllipsisLevelsAreNamedApart) {
  // One parenthesis holds four different constructs, and one refusal name for
  // all of them reported an `if` condition as an argument list.
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n* if (...) foo(E);\n"),
                 "condition ellipsis");
  EXPECT_REFUSED(parsed("@r@\n@@\n- void f(...) {\n- foo();\n- }\n"),
                 "parameter-level ellipsis");
  EXPECT_REFUSED(parsed("@r@\nidentifier a;\n@@\n"
                        "  void __attribute__((...,1,...)) f\n- (int a)\n"
                        "+ ()\n  {...}\n"),
                 "attribute-argument ellipsis");
  // Dots with a named term on each side need a position counted from each
  // end, which the unifier does not do. The suffix form `f(..., E)` is fine
  // now that matching walks the tree instead of emitting positional matchers.
  EXPECT_REFUSED(parsed("@r@\nexpression E, F;\n@@\n- f(E, ..., F);\n"),
                 "argument-level ellipsis between two named arguments");
  EXPECT_REFUSED(parsed("@r@\nidentifier x;\n@@\n* int x[] = { ... };\n"),
                 "initialiser-level ellipsis");
  EXPECT_REFUSED(parsed("@r@\nidentifier s, x;\n@@\n"
                        "* struct s { ... int x; ... };\n"),
                 "field-level ellipsis");
  EXPECT_REFUSED(parsed("@r@\nidentifier e;\n@@\n* enum e { ... };\n"),
                 "enumerator-level ellipsis");
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n* E == ...\n"),
                 "expression-level ellipsis");
  EXPECT_REFUSED(parsed("@r@\nidentifier x;\n@@\n- x[...]\n+ x\n"),
                 "array-size ellipsis");
  // A field-level ellipsis written on a line of its own is decided by the
  // brace it sits in, not by the line.
  EXPECT_REFUSED(parsed("@r@\n@@\n  struct {\n  ...\n- int x;\n+ bool x;\n"
                        "  ...\n  };\n"),
                 "field-level ellipsis");
  EXPECT_REFUSED(parsed("@r exists@\nexpression E;\n@@\n  foo(E);\n"
                        "  <... bar(E); ...>\n- baz(E);\n"),
                 "nested dots <... ...>");
  EXPECT_REFUSED(parsed("@r exists@\nexpression E;\n@@\n  foo(E);\n"
                        "  <+... bar(E); ...+>\n- baz(E);\n"),
                 "nested dots <... ...>");

  // The three shapes the compiler emits are no longer refused at all. A
  // refusal here would be the tool declining what it can do, which the counts
  // would report as an unsupported construct.
  EXPECT_TRUE(parsed("@r@\n@@\n- f(...);\n").fullyUnderstood());
  EXPECT_TRUE(
      parsed("@r@\nexpression E;\n@@\n- f(E, ...);\n").fullyUnderstood());
  EXPECT_TRUE(
      parsed("@r@\nexpression E;\n@@\n- f(..., E, ...);\n").fullyUnderstood());
  EXPECT_TRUE(
      parsed("@r@\nexpression E;\n@@\n- f(..., E);\n").fullyUnderstood());
  // A body on the next line is what makes a bodyless `f(...)` a definition
  // rather than a call, so the level cannot be read off one line.
  EXPECT_REFUSED(parsed("@r@\nidentifier fn;\n@@\nfn(...)\n{\n- foo();\n}\n"),
                 "parameter-level ellipsis");
  // A position can sit between the closing parenthesis and the body.
  EXPECT_REFUSED(parsed("@r@\ntype T;\nposition c;\n@@\n"
                        "T main(...)@c {\n- foo();\n}\n"),
                 "parameter-level ellipsis");
}

TEST(SmplParser, BodyConstructsOutsideTheSubset) {
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n* \\(foo(E)\\|bar(E)\\)\n"),
                 "backslash disjunction \\( \\| \\)");
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n(\n* foo(E);\n&\n"
                        "* bar(E);\n)\n"),
                 "conjunction ( & )");
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n  ctx(E);\n?- foo(E);\n"),
                 "? optional line marker");
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n  ctx(E);\n++ foo(E);\n"),
                 "++ line marker");
  EXPECT_REFUSED(parsed("@r@\n@@\n--- a.c\n+++ b.c\n"),
                 "--- / +++ filespec header");
  EXPECT_REFUSED(parsed("@r@\nidentifier f;\n@@\n* #define f(x) x\n"),
                 "preprocessor directive pattern");
  EXPECT_REFUSED(parsed("@r@\nexpression E;\n@@\n* switch (E) {\n* }\n"),
                 "switch / case pattern");
  EXPECT_REFUSED(parsed("@r@\nidentifier l;\n@@\n* goto l;\n"), "goto pattern");
  EXPECT_REFUSED(parsed("@r@\nidentifier T, f;\n@@\n- T f(int x) {\n- }\n"),
                 "function definition pattern");
  EXPECT_REFUSED(parsed("@r@\nidentifier T, f;\n@@\n- T f(int x);\n"),
                 "function prototype pattern");
  EXPECT_REFUSED(parsed("@r@\nidentifier x;\n@@\n* new x;\n"),
                 "C++ construct in the pattern");
}

TEST(SmplParser, ArgumentLevelWhenIsRejected) {
  // The manual documents it and the production is commented out in the
  // parser, in 1.1.1 and in 1.3.3 both.
  EXPECT_NE(std::string::npos,
            rejection("@r@\nidentifier f;\nexpression E;\n@@\n"
                      "* f(E, ... when != 0);\n")
                .find("when"));
}

TEST(SmplParser, TheMetaVarNameReuseTrap) {
  // A name declared in another rule and used unqualified here is literal C
  // text to Coccinelle, which only warns and matches nothing useful.
  SemanticPatch P = parsed("@r1@\nexpression E;\n@@\n foo(E);\n\n"
                           "@r2@\n@@\n* bar(E);\n");
  EXPECT_REFUSED(P, "metavariable name used as a plain C identifier");
}

TEST(SmplParser, CommentsAndStringsDoNotMoveColumnZero) {
  // A '*/' closing a block comment in column 0 is not a '*' marker, and a
  // '-' inside a string is not one either.
  SemanticPatch P = parsed("@r@\nexpression E;\n@@\n/* a comment\n"
                           "   spanning lines\n*/\n- foo(E);\n");
  ASSERT_EQ(1u, P.Rules[0].Body.size());
  EXPECT_EQ(ItemMarker::Minus, P.Rules[0].Body[0].Marker);
  EXPECT_EQ("foo(E);", P.Rules[0].Body[0].Text);
}

//===----------------------------------------------------------------------===//
// Tier 2: real kernel rules inside the subset, asserted on structure.
//
// All four are the tier-1 rows of test/Inputs/corpus-manifest.tsv for the
// kernel corpus. Provenance is in each file's header; they come from
// linux/scripts/coccinelle at 28924df2a08f.
//===----------------------------------------------------------------------===//

TEST(SmplParserKernel, ErrCastParsesThreeModeVariants) {
  SemanticPatch P = parsed(readInput("cocci/kernel/api/err_cast.cocci"));
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  ASSERT_EQ(4u, P.Virtuals.size());
  ASSERT_EQ(3u, P.Rules.size());
  // The three rules are the context, patch and report variants of one
  // pattern, each gated on the virtuals.
  for (const Rule &R : P.Rules) {
    EXPECT_TRUE(R.DependsOnVirtual) << R.Name;
    EXPECT_FALSE(R.DependsOn.empty()) << R.Name;
    ASSERT_FALSE(R.MetaVars.empty()) << R.Name;
    EXPECT_EQ(MetaKind::Expression, R.MetaVars[0].Kind);
    EXPECT_EQ("x", R.MetaVars[0].Name);
  }
  EXPECT_EQ(ItemMarker::Star, P.Rules[0].Body[0].Marker);
  EXPECT_EQ("ERR_PTR(PTR_ERR(x))", P.Rules[0].Body[0].Text);
  ASSERT_EQ(2u, P.Rules[1].Body.size());
  EXPECT_EQ(ItemMarker::Minus, P.Rules[1].Body[0].Marker);
  EXPECT_EQ(ItemMarker::Plus, P.Rules[1].Body[1].Marker);
  EXPECT_EQ("ERR_CAST(x)", P.Rules[1].Body[1].Text);
  // The report rule binds a position on the ERR_PTR token.
  const Rule &Rep = P.Rules[2];
  EXPECT_EQ("r", Rep.Name);
  ASSERT_EQ(2u, Rep.MetaVars.size());
  EXPECT_EQ(MetaKind::Position, Rep.MetaVars[1].Kind);
  ASSERT_EQ(1u, Rep.Body.size());
  EXPECT_EQ("p", Rep.Body[0].PositionVar);
  EXPECT_EQ(ItemMarker::Context, Rep.Body[0].Marker);
}

TEST(SmplParserKernel, KfreeaddrParsesADisjunctionOfTwoBranches) {
  SemanticPatch P = parsed(readInput("cocci/kernel/free/kfreeaddr.cocci"));
  ASSERT_EQ(1u, P.Rules.size());
  const Rule &R = P.Rules[0];
  EXPECT_EQ("r", R.Name);
  EXPECT_TRUE(R.DependsOnVirtual);
  ASSERT_EQ(1u, R.Body.size());
  const PatternItem &D = R.Body[0];
  EXPECT_EQ(ItemKind::Disjunction, D.Kind);
  ASSERT_EQ(2u, D.Branches.size());
  ASSERT_EQ(1u, D.Branches[0].size());
  ASSERT_EQ(1u, D.Branches[1].size());
  EXPECT_EQ("kfree(&e->f)", D.Branches[0][0].Text);
  EXPECT_EQ("kfree_sensitive(&e->f)", D.Branches[1][0].Text);
  for (const auto &B : D.Branches) {
    EXPECT_EQ(ItemMarker::Star, B[0].Marker);
    EXPECT_EQ("p", B[0].PositionVar);
  }
  // The only refusal is in the org-mode script rule, which prints through
  // cocci.print_main rather than coccilib.report.
  ASSERT_EQ(1u, P.Refusals.size());
  EXPECT_EQ("coccilib call other than print_report/print_todo",
            P.Refusals[0].Construct);
}

TEST(SmplParserKernel, ReturnvarParsesTypeMetaVarAndWhenStrict) {
  SemanticPatch P = parsed(readInput("cocci/kernel/misc/returnvar.cocci"));
  ASSERT_EQ(3u, P.Rules.size());
  const Rule &Rep = P.Rules[2];
  EXPECT_EQ("r1", Rep.Name);
  ASSERT_EQ(5u, Rep.MetaVars.size());
  EXPECT_EQ(MetaKind::Type, Rep.MetaVars[0].Kind);
  EXPECT_EQ("T", Rep.MetaVars[0].Name);
  EXPECT_EQ(MetaKind::Constant, Rep.MetaVars[1].Kind);
  EXPECT_EQ(MetaKind::Identifier, Rep.MetaVars[2].Kind);
  ASSERT_EQ(3u, Rep.Body.size());
  EXPECT_EQ("T ret = C;", Rep.Body[0].Text);
  EXPECT_EQ("p1", Rep.Body[0].PositionVar);
  // The ellipsis carries `when != ret` on one line and `when strict` on the
  // next, which is the only way to write two clauses.
  const PatternItem &Dots = Rep.Body[1];
  EXPECT_EQ(ItemKind::Dots, Dots.Kind);
  ASSERT_EQ(1u, Dots.WhenNot.size());
  EXPECT_EQ("ret", Dots.WhenNot[0]);
  EXPECT_TRUE(Dots.WhenStrict);
  EXPECT_FALSE(Dots.WhenAny);
  EXPECT_EQ("return ret;", Rep.Body[2].Text);
  EXPECT_EQ("p2", Rep.Body[2].PositionVar);
}

TEST(SmplParserKernel, SecsToJiffiesParsesSixRules) {
  SemanticPatch P =
      parsed(readInput("cocci/kernel/misc/secs_to_jiffies.cocci"));
  EXPECT_TRUE(P.fullyUnderstood()) << refusalList(P);
  ASSERT_EQ(6u, P.Rules.size());
  // Four patch rules rewrite a call, then a report rule and a context rule
  // hold the same alternatives in a disjunction.
  EXPECT_EQ("pconst", P.Rules[0].Name);
  ASSERT_EQ(2u, P.Rules[0].Body.size());
  EXPECT_EQ(ItemMarker::Minus, P.Rules[0].Body[0].Marker);
  EXPECT_EQ("msecs_to_jiffies(C * 1000)", P.Rules[0].Body[0].Text);
  EXPECT_EQ("secs_to_jiffies(C)", P.Rules[0].Body[1].Text);
  EXPECT_EQ(MetaKind::Constant, P.Rules[0].MetaVars[0].Kind);
  const Rule &Rep = P.Rules[4];
  EXPECT_EQ("r", Rep.Name);
  ASSERT_EQ(1u, Rep.Body.size());
  EXPECT_EQ(ItemKind::Disjunction, Rep.Body[0].Kind);
  EXPECT_LE(2u, Rep.Body[0].Branches.size());
}

//===----------------------------------------------------------------------===//
// Tier 3: real kernel rules outside the subset, asserted on refusal names.
//===----------------------------------------------------------------------===//

TEST(SmplParserKernel, KmallocObjsNamesEveryRefusedConstruct) {
  // The rule behind the largest tree-wide change in kernel history. Every
  // construct the brief calls out has to come back named.
  SemanticPatch P = parsed(readInput("cocci/kernel/api/kmalloc_objs.cocci"));
  EXPECT_FALSE(P.fullyUnderstood());
  expectWellFormedRefusals(P);
  EXPECT_REFUSED(P, "@initialize:python@ / @finalize:python@ rule");
  EXPECT_REFUSED(P, "set-valued metavariable constraint");
  EXPECT_REFUSED(P, "metavariable typed by another metavariable");
  EXPECT_REFUSED(P, "metavariable restricted to a concrete C type");
  EXPECT_REFUSED(P, "regex constraint on a metavariable");
  EXPECT_REFUSED(P, "fresh identifier");
  EXPECT_REFUSED(P, "fresh identifier with ## concatenation");
  EXPECT_REFUSED(P, "python constraint on a metavariable");
  EXPECT_REFUSED(P, "depends on file in");
  EXPECT_REFUSED(P, "backslash disjunction \\( \\| \\)");
  EXPECT_REFUSED(P, "argument-level ellipsis continued on another line");
}

TEST(SmplParserKernel, BadzeroNeedsOcaml) {
  SemanticPatch P = parsed(readInput("cocci/kernel/null/badzero.cocci"));
  EXPECT_REFUSED(P, "@initialize:ocaml@ / @finalize:ocaml@ rule");
  EXPECT_REFUSED(P, "script:ocaml rule");
  EXPECT_REFUSED(P, "disable <isomorphism>");
  EXPECT_REFUSED(P, "nested dots <... ...>");
  expectWellFormedRefusals(P);
}

TEST(SmplParserKernel, CheckBq27xxxNeedsOcaml) {
  SemanticPatch P =
      parsed(readInput("cocci/kernel/api/check_bq27xxx_data.cocci"));
  EXPECT_REFUSED(P, "@initialize:ocaml@ / @finalize:ocaml@ rule");
  EXPECT_REFUSED(P, "script:ocaml rule");
  EXPECT_REFUSED(P, "initializer list metavariable");
  expectWellFormedRefusals(P);
}

TEST(SmplParserKernel, ClkPutNeedsWhenForallAndANest) {
  SemanticPatch P = parsed(readInput("cocci/kernel/free/clk_put.cocci"));
  EXPECT_REFUSED(P, "when forall");
  EXPECT_REFUSED(P, "nested dots <... ...>");
  EXPECT_REFUSED(P, "condition ellipsis");
  expectWellFormedRefusals(P);
}

TEST(SmplParserKernel, EnoNeedsBackslashDisjunction) {
  // Named as tier 1 in the first scoping pass, and it is not: the allocator
  // list is a backslash disjunction. Its argument-level dots are inside the
  // subset now, so the disjunction is the whole of what stops it.
  SemanticPatch P = parsed(readInput("cocci/kernel/null/eno.cocci"));
  EXPECT_REFUSED(P, "backslash disjunction \\( \\| \\)");
  // The statement-level ellipsis and its `when !=` are still understood.
  bool SawDots = false;
  for (const Rule &R : P.Rules)
    for (const PatternItem &It : R.Body)
      if (It.Kind == ItemKind::Dots && It.WhenNot.size() == 1 &&
          It.WhenNot[0] == "x = E")
        SawDots = true;
  EXPECT_TRUE(SawDots) << "the `... when != x = E` was not parsed";
}

//===----------------------------------------------------------------------===//
// Tier 4: the conformance sweep over every native sample in the corpus.
//===----------------------------------------------------------------------===//

namespace {

/// One row of test/Inputs/corpus-manifest.tsv, keyed by the path relative to
/// test/Inputs.
struct ManifestRow {
  unsigned Tier = 0;
  std::string ExpectedRefusals;
};

/// Reads the manifest. Empty when it is not there, which the sweep reports
/// rather than working around.
StringMap<ManifestRow> readManifest() {
  StringMap<ManifestRow> Rows;
  SmallString<256> Path(testInputsDir());
  sys::path::append(Path, "corpus-manifest.tsv");
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf)
    return Rows;
  SmallVector<StringRef, 32> Lines;
  (*Buf)->getBuffer().split(Lines, '\n');
  for (StringRef Line : Lines) {
    if (Line.empty() || Line.front() == '#')
      continue;
    SmallVector<StringRef, 24> Fields;
    Line.split(Fields, '\t');
    if (Fields.size() < 11 || Fields[0] == "path")
      continue;
    unsigned Tier = 0;
    if (Fields[2].getAsInteger(10, Tier) || Tier == 0)
      continue;
    Rows[Fields[0]] = ManifestRow{Tier, Fields[10].str()};
  }
  return Rows;
}

enum class Outcome { Clean, Refused, Rejected, Timeout, Crash };

/// Parses \p Text on another thread and gives up after \p Seconds, so that a
/// parser that hangs is a counted outcome rather than a hung test. The state
/// is leaked on a timeout because the worker still owns it.
struct SweepResult {
  Outcome Out = Outcome::Crash;
  std::optional<SemanticPatch> Patch;
  std::string Error;
};

SweepResult parseWithTimeout(StringRef Text, StringRef Name, unsigned Seconds) {
  struct Shared {
    std::mutex M;
    std::condition_variable CV;
    bool Done = false;
    SweepResult R;
    std::string Text, Name;
  };
  auto *S = new Shared();
  S->Text = Text.str();
  S->Name = Name.str();
  std::thread([S] {
    SweepResult R;
    std::string Error;
    auto P = parseSemanticPatch(S->Text, S->Name, Error);
    if (!P) {
      R.Out = Outcome::Rejected;
      R.Error = Error;
    } else {
      R.Out = P->fullyUnderstood() ? Outcome::Clean : Outcome::Refused;
      R.Patch = std::move(P);
    }
    std::lock_guard<std::mutex> G(S->M);
    S->R = std::move(R);
    S->Done = true;
    S->CV.notify_one();
  }).detach();

  std::unique_lock<std::mutex> G(S->M);
  if (!S->CV.wait_for(G, std::chrono::seconds(Seconds),
                      [S] { return S->Done; })) {
    SweepResult R;
    R.Out = Outcome::Timeout;
    G.unlock();
    return R; // Shared is deliberately leaked: the worker still owns it.
  }
  SweepResult R = std::move(S->R);
  G.unlock();
  delete S;
  return R;
}

/// Removes comment text, keeping every other column and every line break, so
/// that the checks below see the same layout the parser does. Written
/// separately from the parser's own version on purpose: a bug shared by both
/// would otherwise hide.
std::string blankCommentsForCheck(StringRef In) {
  std::string Buf = In.str();
  enum { Code, Block, Str } St = Code;
  for (size_t I = 0, E = Buf.size(); I != E; ++I) {
    char C = Buf[I];
    if (St == Code) {
      if (C == '/' && I + 1 != E && Buf[I + 1] == '/') {
        while (I != E && Buf[I] != '\n')
          Buf[I++] = ' ';
        --I;
      } else if (C == '/' && I + 1 != E && Buf[I + 1] == '*') {
        Buf[I] = Buf[I + 1] = ' ';
        ++I;
        St = Block;
      } else if (C == '"') {
        St = Str;
      }
    } else if (St == Block) {
      if (C == '*' && I + 1 != E && Buf[I + 1] == '/') {
        Buf[I] = Buf[I + 1] = ' ';
        ++I;
        St = Code;
      } else if (C != '\n') {
        Buf[I] = ' ';
      }
    } else if (C == '"' || C == '\n') {
      St = Code;
    }
  }
  return Buf;
}

/// Constructs that are certainly outside the subset, checked on the source
/// text. A file that parses with an empty refusal list must contain none of
/// them: that is the test for a silent misparse.
/// Options a `#spatch` line may carry without changing what a pattern means.
/// Written out again here rather than shared with the parser, so that a
/// mistake in the parser's list cannot hide behind the same mistake here.
bool isInertSpatchOption(StringRef Opt) {
  static const char *const Inert[] = {"-o",
                                      "-U",
                                      "--in-place",
                                      "--out-place",
                                      "--suffix",
                                      "--show-diff",
                                      "--no-show-diff",
                                      "--force-diff",
                                      "--keep-comments",
                                      "--linux-spacing",
                                      "--smpl-spacing",
                                      "--indent",
                                      "--max-width",
                                      "--patch",
                                      "--selected-only",
                                      "--quiet",
                                      "--very-quiet",
                                      "--debug",
                                      "--pad",
                                      "--profile",
                                      "--profile-per-file",
                                      "--bench",
                                      "--track-iso",
                                      "--profile-iso",
                                      "--graphical-trace",
                                      "--gt-without-label",
                                      "--disable-once",
                                      "--show-trace-profile",
                                      "--print-options-only",
                                      "--parse-error-msg",
                                      "--type-error-msg",
                                      "--verbose-match",
                                      "--verbose-engine",
                                      "--verbose-ctl-engine",
                                      "--verbose-parsing",
                                      "--verbose-includes",
                                      "--show-trying",
                                      "--show-dependencies",
                                      "--show-bindings",
                                      "--show-transinfo",
                                      "--show-misc",
                                      "--show-flow",
                                      "--show-c",
                                      "--show-cocci",
                                      "--show-SP",
                                      "--cocci-internals",
                                      "--c-internals",
                                      "--debug-cpp",
                                      "--debug-lexer",
                                      "--debug-etdt",
                                      "--debug-typedef",
                                      "--debug-unparsing",
                                      "--debug-parse-cocci",
                                      "--filter-msg",
                                      "--filter-define-error",
                                      "--filter-msg-define-error",
                                      "--filter-passed-level",
                                      "--jobs",
                                      "-j",
                                      "--chunksize",
                                      "--tmp-dir",
                                      "--temp-files",
                                      "--index",
                                      "--max",
                                      "--mod-distrib",
                                      "--use-cache",
                                      "--cache-prefix",
                                      "--cache-limit",
                                      "--no-include-cache",
                                      "--save-tmp-files",
                                      "--batch_mode",
                                      "--timeout",
                                      "--no-scanner",
                                      "--disable-worth-trying-opt"};
  for (const char *K : Inert)
    if (Opt == K)
      return true;
  return false;
}

/// True when every option on a `#spatch` line is inert, which is the only
/// form of the line the subset accepts.
bool spatchLineIsInert(StringRef Line) {
  StringRef Rest = Line.ltrim().drop_front(1).ltrim();
  if (!Rest.starts_with("spatch"))
    return false;
  Rest = Rest.drop_front(6);
  while (true) {
    Rest = Rest.ltrim();
    if (Rest.empty())
      return true;
    size_t N = 0;
    while (N != Rest.size() && !isSpace(Rest[N]))
      ++N;
    StringRef Opt = Rest.take_front(N);
    Rest = Rest.drop_front(N);
    if (Opt.starts_with("-") && !isInertSpatchOption(Opt))
      return false;
  }
}

/// Constructs that are certainly outside the subset, checked on the source
/// text. A file that parses with an empty refusal list must contain none of
/// them: that is the test for a silent misparse. \p InDecls says whether the
/// line sits in a metavariable declaration block, because a declaration and
/// a pattern line that read alike mean different things.
const char *outOfSubsetToken(StringRef Line, bool InDecls) {
  static const std::pair<const char *, const char *> Substrings[] = {
      {"\\(", "backslash disjunction"},
      {"\\|", "backslash disjunction"},
      {"\\&", "backslash conjunction"},
      {"<...", "nest"},
      {"...>", "nest"},
      {"script:ocaml", "ocaml script"},
      {"script:python(", "python constraint"},
      {"initialize:", "initialize rule"},
      {"finalize:", "finalize rule"},
      {"=~", "regex constraint"},
      {"!~", "regex constraint"},
      {"##", "identifier concatenation"},
      {"fresh identifier", "fresh identifier"},
      {"when forall", "when forall"},
      {"when exists", "when exists"},
      {"when =", "when = code"},
      {"using \"", "isomorphism file"},
      {"[[", "C++ attribute"},
      {"--c++", "#spatch --c++"},
      {"virtual.", "virtual.x value"},
      {"merge.", "script merge variable"},
      {" extends ", "extends"},
      {" disable ", "disable iso"},
      {"file in ", "depends on file in"},
  };
  for (const auto &S : Substrings)
    if (Line.contains(S.first))
      return S.second;
  StringRef Trimmed = Line.ltrim();
  static const char *const DeclKinds[] = {
      "iterator ",           "declarer ",       "metavariable ",
      "idexpression",        "pragmainfo ",     "comments ",
      "attribute ",          "position any",    "expression list ",
      "identifier list ",    "parameter list ", "statement list ",
      "field list ",         "format list ",    "binary operator ",
      "assignment operator "};
  for (const char *K : DeclKinds)
    if (Trimmed.starts_with(K))
      return "out-of-subset metavariable kind";
  if (InDecls) {
    // A metavariable whose type is written out, in any of its spellings.
    static const char *const CTypes[] = {
        "int ",      "char ",   "void ",  "long ",   "short ",
        "unsigned ", "signed ", "float ", "double ", "_Bool ",
        "struct ",   "union ",  "enum ",  "const ",  "volatile "};
    for (const char *K : CTypes)
      if (Trimmed.starts_with(K))
        return "metavariable restricted to a concrete C type";
    if (Trimmed.contains("[]") || Trimmed.contains("[ ]"))
      return "array-typed metavariable declaration";
    if (Trimmed.contains("<="))
      return "metavariable bound to a subterm of an inherited one";
    if (Trimmed.contains(":"))
      return "constrained metavariable declaration";
  }
  if (Line.starts_with("?") || Line.starts_with("++") ||
      Line.starts_with("---") || Line.starts_with("+++"))
    return "line marker outside the subset";
  // A '#spatch' line carrying only inert options is in the subset; every
  // other line starting with '#' is not.
  if (Trimmed.starts_with("#") && !spatchLineIsInert(Trimmed))
    return "preprocessor line";
  // An indented '-' or '*' followed by a space is the marker trap.
  if (!Line.empty() && isSpace(Line.front())) {
    StringRef LT = Line.ltrim();
    if (LT.size() > 1 && isSpace(LT[1]) &&
        (LT.front() == '-' || LT.front() == '*'))
      return "indented line marker";
    if (LT == "|" || LT == "&")
      return "indented disjunction delimiter";
  }
  return nullptr;
}

/// Names every rule and branch item in \p Items, so the checks below see the
/// whole body including the inside of a disjunction.
void collectItems(const std::vector<PatternItem> &Items,
                  std::vector<const PatternItem *> &Out) {
  for (const PatternItem &It : Items) {
    Out.push_back(&It);
    for (const auto &B : It.Branches)
      collectItems(B, Out);
  }
}

} // namespace

TEST(SmplParserSweep, EveryNativeSampleLandsInOneOfThreeStates) {
  std::string Root = testInputsDir();
  SmallString<256> CorpusDir(Root);
  sys::path::append(CorpusDir, "cocci");
  std::error_code EC;
  std::vector<std::string> Files;
  for (sys::fs::recursive_directory_iterator It(CorpusDir, EC), End;
       It != End && !EC; It.increment(EC))
    if (StringRef(It->path()).ends_with(".cocci"))
      Files.push_back(It->path());
  std::sort(Files.begin(), Files.end());
  ASSERT_FALSE(Files.empty())
      << "no .cocci samples under " << CorpusDir.c_str();

  StringMap<ManifestRow> Manifest = readManifest();
  EXPECT_FALSE(Manifest.empty()) << "corpus-manifest.tsv is missing, so the "
                                    "expected outcome per file is unknown";

  unsigned Clean = 0, Refused = 0, Rejected = 0, Timeout = 0;
  unsigned ScannerFiredOnRefused = 0;
  unsigned TierOneWithRefusals = 0, TierTwoOrThreeClean = 0;
  std::string TierOneNames, TierTwoOrThreeCleanNames;

  // Two rows of the manifest are false positives of the regex classifier
  // that built its tier column: elsify.cocci's `else GOTO(e2);` is read as a
  // function prototype, and notnot.cocci's `- ? true : false` is read as a
  // '?' line marker when it is the tail of a conditional expression. Both
  // parse clean here, and Coccinelle accepts both.
  const StringRef ClassifierFalsePositives[] = {
      "cocci/coccinelle/tests/elsify.cocci",
      "cocci/coccinelle/tests/notnot.cocci"};

  // Constructs the subset took in after the corpus manifest was written, so
  // a file the manifest blocks only on these is expected to parse clean now.
  //
  // The last entry is the name that used to cover five levels at once. It is
  // whitelisted whole because the manifest records only that name, and a file
  // wrongly freed under it is still caught by the named-or-compiles check
  // above, which no name in this list can satisfy on its own.
  const StringRef SupportedSinceManifest[] = {
      "symbol declaration", "typedef declaration",
      "ellipsis inside a one-line block", "#spatch embedded options",
      "argument-level or parameter-level ellipsis"};

  for (const std::string &File : Files) {
    StringRef Rel = StringRef(File).drop_front(Root.size());
    Rel = Rel.ltrim("/\\");
    auto Buf = MemoryBuffer::getFile(File);
    ASSERT_TRUE(static_cast<bool>(Buf)) << "cannot read " << File;
    StringRef Text = (*Buf)->getBuffer();

    SweepResult R = parseWithTimeout(Text, Rel, /*Seconds=*/10);
    switch (R.Out) {
    case Outcome::Timeout:
      ++Timeout;
      ADD_FAILURE() << Rel << ": the parser did not finish in 10s";
      continue;
    case Outcome::Crash:
      ADD_FAILURE() << Rel << ": the parse did not report an outcome";
      continue;
    case Outcome::Rejected:
      ++Rejected;
      EXPECT_FALSE(R.Error.empty())
          << Rel << ": rejected with no error message";
      break;
    case Outcome::Clean:
      ++Clean;
      break;
    case Outcome::Refused:
      ++Refused;
      break;
    }

    if (R.Patch) {
      // Every refusal is usable: a name a reader recognises, a reason, and a
      // line to look at.
      for (const Refusal &Rf : R.Patch->Refusals) {
        EXPECT_FALSE(Rf.Construct.empty()) << Rel;
        EXPECT_FALSE(Rf.Reason.empty()) << Rel << ": " << Rf.Construct;
        EXPECT_NE(0u, Rf.Line) << Rel << ": " << Rf.Construct;
      }
      // An ellipsis that reaches a statement's text must either be named by
      // a refusal or compile to a matcher. Naming alone was the earlier bar
      // and it let a pattern through that reported NOT COMPILED, which is the
      // silent misparse this scanner exists to catch.
      for (const Rule &Rule : R.Patch->Rules) {
        std::vector<const PatternItem *> Items;
        collectItems(Rule.Body, Items);
        for (const PatternItem *It : Items) {
          if (It->Kind != ItemKind::Statement ||
              !StringRef(It->Text).contains("..."))
            continue;
          bool Named = false;
          for (const Refusal &Rf : R.Patch->Refusals)
            if (Rf.Line == It->Line &&
                StringRef(Rf.Construct).contains("ellipsis"))
              Named = true;
          if (Named)
            continue;
          std::string Error;
          std::optional<ParsedPattern> Parsed = parsePattern(
              Rule.MetaVars, {It->Text}, R.Patch->TypeNames, Error);
          const bool Compiled = Parsed && Parsed->Items[0];
          if (Parsed && !Parsed->Items[0])
            Error = Parsed->Errors[0];
          EXPECT_TRUE(Compiled)
              << Rel << ":" << It->Line
              << ": an ellipsis reached a statement's text with no refusal "
                 "naming its level and no matcher to run: "
              << It->Text << " [" << Error << "]";
        }
      }
      // A `@script:` rule has no pattern and is never compiled, so it is
      // counted separately. A file that has one and records none would be
      // reporting success while a rule left no trace.
      // A header spells it `script:<lang>` and may put the '@' on an
      // earlier line, while a metavariable constraint spells it
      // `script:<lang>(`, so the parenthesis is what tells them apart.
      unsigned ScriptHeaders = 0;
      {
        std::string Blanked = blankCommentsForCheck(Text);
        StringRef Rest(Blanked);
        while (true) {
          size_t At = Rest.find("script:");
          if (At == StringRef::npos)
            break;
          Rest = Rest.drop_front(At + 7);
          StringRef After = Rest;
          while (!After.empty() &&
                 (isAlnum(After.front()) || After.front() == '_'))
            After = After.drop_front();
          if (!After.ltrim(" \t").starts_with("("))
            ++ScriptHeaders;
        }
      }
      EXPECT_EQ(ScriptHeaders, R.Patch->ScriptRules.size())
          << Rel << ": the file has " << ScriptHeaders
          << " script rule header(s) and the patch records "
          << R.Patch->ScriptRules.size();

      // A clean parse is a promise that nothing was dropped, so the source
      // must hold no construct that is certainly outside the subset. The same
      // scan runs over the refused files and the count is reported, because a
      // scanner that has stopped firing proves nothing about the clean ones.
      {
        std::string Blanked = blankCommentsForCheck(Text);
        SmallVector<StringRef, 64> Lines;
        StringRef(Blanked).split(Lines, '\n');
        enum { Body, Decls, ScriptDecls } State = Body;
        for (StringRef Line : Lines) {
          StringRef T = Line.trim();
          bool Header =
              !T.empty() && T.front() == '@' && T.ends_with("@") && T != "@@";
          bool InDecls = State == Decls && !Header;
          if (T.starts_with("@@"))
            State = State == Body ? Decls : Body;
          else if (Header)
            State = T.contains("script:") || T.contains("initialize:") ||
                            T.contains("finalize:")
                        ? ScriptDecls
                        : Decls;
          const char *What = outOfSubsetToken(Line.rtrim('\r'), InDecls);
          if (!What)
            continue;
          if (R.Patch->fullyUnderstood())
            ADD_FAILURE() << Rel
                          << ": parsed with no refusals, but the "
                             "source holds a "
                          << What << ": " << Line.trim();
          else
            ++ScannerFiredOnRefused;
          break;
        }
      }
    }

    auto Row = Manifest.find(Rel);
    if (Row == Manifest.end())
      continue;
    unsigned Tier = Row->second.Tier;
    // A tier-1 file is inside the subset, so it must parse.
    if (Tier == 1)
      EXPECT_NE(Outcome::Rejected, R.Out)
          << Rel
          << ": the manifest calls it tier 1, but it was rejected: " << R.Error;
    if (Tier == 1 && R.Out == Outcome::Refused) {
      ++TierOneWithRefusals;
      TierOneNames += ("\n    " + Rel + ": " + refusalList(*R.Patch)).str();
    }
    if (Tier != 1 && R.Out == Outcome::Clean) {
      bool Known = false;
      for (StringRef FP : ClassifierFalsePositives)
        if (Rel == FP)
          Known = true;
      // The manifest's tier column records the subset as it was when the
      // corpus was built. A file it blocks only on constructs the subset has
      // since taken in is expected to be clean now, and the check still bites
      // for a file blocked on anything else.
      SmallVector<StringRef, 8> Expected;
      StringRef(Row->second.ExpectedRefusals).split(Expected, " | ");
      bool OnlySinceSupported = !Expected.empty();
      for (StringRef E : Expected) {
        E = E.trim();
        if (E.empty()) {
          OnlySinceSupported = false;
          continue;
        }
        bool Supported = false;
        for (StringRef S : SupportedSinceManifest)
          if (E == S)
            Supported = true;
        if (!Supported)
          OnlySinceSupported = false;
      }
      if (OnlySinceSupported)
        Known = true;
      if (!Known) {
        ++TierTwoOrThreeClean;
        TierTwoOrThreeCleanNames += ("\n    " + Rel + ": manifest expects " +
                                     Row->second.ExpectedRefusals)
                                        .str();
      }
    }
  }

  outs() << "\n  clang-spatch SmPL conformance sweep, " << Files.size()
         << " native samples\n"
         << "  ----------------------------------------------------------\n"
         << formatv("  {0,-28} {1,6}\n", "parsed clean", Clean)
         << formatv("  {0,-28} {1,6}\n", "parsed with refusals", Refused)
         << formatv("  {0,-28} {1,6}\n", "rejected with an error", Rejected)
         << formatv("  {0,-28} {1,6}\n", "timed out", Timeout)
         << formatv("  {0,-28} {1,6}\n", "crashed", 0)
         << "  ----------------------------------------------------------\n"
         << formatv("  {0,-28} {1,6}\n", "refused, scanner agrees",
                    ScannerFiredOnRefused)
         << formatv("  {0,-28} {1,6}\n", "tier-1 rows with refusals",
                    TierOneWithRefusals)
         << TierOneNames << "\n";

  EXPECT_EQ(Files.size(), Clean + Refused + Rejected + Timeout);
  EXPECT_EQ(0u, Timeout);
  // A file the manifest puts outside the subset must not come back clean:
  // that is the silent partial-handling failure this tool exists to prevent.
  EXPECT_EQ(0u, TierTwoOrThreeClean)
      << "files the manifest puts outside the subset parsed with no "
         "refusals:"
      << TierTwoOrThreeCleanNames;
  // Refusing more than the manifest expects is safe, and each one is
  // accounted for in the parser's notes, so this only guards against a
  // regression that starts refusing the subset wholesale.
  EXPECT_LE(TierOneWithRefusals, 16u) << TierOneNames;
}

TEST(SmplParser, ThePatternStatementIsTheUnitRatherThanTheLine) {
  // A rule body is written a line at a time and a pattern is not. Before the
  // lines were joined, `demos/itimer.cocci` reached the pattern parser as an
  // `if` with no body and a separate orphan assignment.
  SemanticPatch P = parsed("@r@\n@@\n"
                           "- if (cputime_eq(a, b))\n"
                           "-   a = jiffies_to_cputime(1);\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("if (cputime_eq(a, b)) a = jiffies_to_cputime(1);",
            P.Rules[0].Minus[0].Text);
  EXPECT_TRUE(P.Rules[0].Plus.empty());
}

TEST(SmplParser, AContextLineBelongsToBothSides) {
  // This is what SmPL means by a context line and it is the whole reason the
  // sides are grouped apart. Grouping by marker instead left `if (a)`,
  // `if (b)` and `c();` as three fragments, none of which is a statement the
  // patch contains.
  SemanticPatch P = parsed("@r@\n@@\n- if (a)\n+ if (b)\n  c();\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].Minus.size()) << refusalList(P);
  ASSERT_EQ(1u, P.Rules[0].Plus.size()) << refusalList(P);
  EXPECT_EQ("if (a) c();", P.Rules[0].Minus[0].Text);
  EXPECT_EQ("if (b) c();", P.Rules[0].Plus[0].Text);
  EXPECT_EQ(PatternItem::Marker::Minus, P.Rules[0].Minus[0].Marker);
  EXPECT_EQ(PatternItem::Marker::Plus, P.Rules[0].Plus[0].Marker);
}

TEST(SmplParser, AChangedLineMarksTheStatementItWasJoinedInto) {
  // The group opens on a context line here, so the marker has to come from
  // the line joined into it. Without that the statement reads as unchanged
  // and the runner rewrites nothing.
  SemanticPatch P = parsed("@r@\n@@\n  if (a)\n-   b();\n+   c();\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("if (a) b();", P.Rules[0].Minus[0].Text);
  EXPECT_EQ(PatternItem::Marker::Minus, P.Rules[0].Minus[0].Marker);
  ASSERT_EQ(1u, P.Rules[0].Plus.size()) << refusalList(P);
  EXPECT_EQ("if (a) c();", P.Rules[0].Plus[0].Text);
  EXPECT_EQ(PatternItem::Marker::Plus, P.Rules[0].Plus[0].Marker);
}

TEST(SmplParser, ATypeFragmentTakesTheDeclaratorBelowIt) {
  // `tests/longlong.cocci` is this shape. A run of type keywords carries no
  // declarator, so it cannot be a statement, and the backward test could not
  // tell it from a complete expression pattern: both end in a word.
  SemanticPatch P =
      parsed("@r@\nidentifier x;\n@@\n- long long\n+ int\n  x;\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].Minus.size()) << refusalList(P);
  ASSERT_EQ(1u, P.Rules[0].Plus.size()) << refusalList(P);
  EXPECT_EQ("long long x;", P.Rules[0].Minus[0].Text);
  EXPECT_EQ("int x;", P.Rules[0].Plus[0].Text);
}

TEST(SmplParser, ALineOpeningWithAnOperatorJoinsTheOneAboveIt) {
  // `tests/cptr.cocci` is this shape. The line above ends in an identifier,
  // which is how a complete expression pattern ends too, so what settles it
  // is the line below opening with `=`.
  SemanticPatch P = parsed("@r@\nexpression E;\nidentifier s;\n@@\n"
                           "- const char *s\n+ const char * const s\n"
                           "    = E;\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("const char *s = E;", P.Rules[0].Minus[0].Text);
  EXPECT_EQ("const char * const s = E;", P.Rules[0].Plus[0].Text);
}

TEST(SmplParser, TwoCompleteStatementsAreNotFusedByALeadingOperator) {
  // A line opening with `*` or `.` is a continuation only when the line above
  // did not finish a statement. Without that test these two patterns join
  // into one item holding a sequence.
  SemanticPatch P = parsed("@r@\n@@\n- foo();\n- *p = 0;\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(2u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("foo();", P.Rules[0].Minus[0].Text);
  EXPECT_EQ("*p = 0;", P.Rules[0].Minus[1].Text);
}

TEST(SmplParser, AnOpeningBraceIsNotJoinedToTheStatementInsideIt) {
  // The joined text would leave a brace unclosed, and every item of a rule
  // shares one translation unit, so the imbalance takes the items after it
  // with it. `tests/defineinit.cocci` and `tests/strangeorder.cocci` both
  // lost a later statement metavariable that way.
  SemanticPatch P = parsed("@r@\nexpression E;\n@@\n  {\n- .foo = E\n  }\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_LE(2u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("{", P.Rules[0].Minus[0].Text);
}

TEST(SmplParser, AStatementOpeningWithAnOperatorIsAFragment) {
  // The plus side here is `= E;`, with nothing to assign to. Marking it
  // finished would let the runner write it back over the whole declaration.
  SemanticPatch P =
      parsed("@r@\nexpression E;\n@@\n- const char *s\n    = E;\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].Plus.size()) << refusalList(P);
  EXPECT_EQ("= E;", P.Rules[0].Plus[0].Text);
  EXPECT_TRUE(P.Rules[0].Plus[0].Unfinished);
}

TEST(SmplParser, AnExpressionPatternStandsAloneWithoutASemicolon) {
  // Coccinelle removes an expression by writing it with no terminator, and
  // treating every such line as an unfinished statement joined 842 files'
  // patterns into nonsense.
  SemanticPatch P = parsed("@r@\nexpression E;\nconstant c;\ntype T;\n@@\n"
                           "- kzalloc(c * sizeof(T), E)\n"
                           "+ kcalloc(c, sizeof(T), E)\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].Minus.size()) << refusalList(P);
  ASSERT_EQ(1u, P.Rules[0].Plus.size()) << refusalList(P);
  EXPECT_EQ("kzalloc(c * sizeof(T), E)", P.Rules[0].Minus[0].Text);
  EXPECT_EQ("kcalloc(c, sizeof(T), E)", P.Rules[0].Plus[0].Text);
}

TEST(SmplParser, AnUnclosedBracketTakesTheFollowingLines) {
  SemanticPatch P = parsed("@r@\nexpression E;\n@@\n- foo(\n-   E,\n-   1)\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(1u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("foo( E, 1)", P.Rules[0].Minus[0].Text);
}

TEST(SmplParser, AnElseIsNotJoinedBackwardsOntoTheIfAboveIt) {
  // A dangling `else` takes the line after it, so `else` and its body are one
  // item. It is NOT joined backwards onto a complete `if` branch.
  //
  // Joining backwards looks right and cannot work. A transformed `if` keeps
  // its head on a context line and its branches on `-` and `+` lines, so the
  // statement spans a marker change and no same-marker rule can assemble it.
  // What the backward join did instead was fuse two `-` lines of
  // tests/elsify.cocci into `GOTO(e1); else GOTO(e2);`, which does not parse.
  SemanticPatch P = parsed("@r@\n@@\n- if (a)\n-   b();\n- else\n-   c();\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(2u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("if (a) b();", P.Rules[0].Minus[0].Text);
  EXPECT_EQ("else c();", P.Rules[0].Minus[1].Text);
}

TEST(SmplParser, ABareStatementMetavariableDoesNotAbsorbTheNextLine) {
  // `statement S;` makes S stand for a whole statement, so the line holding
  // it is already complete.
  SemanticPatch P = parsed("@r@\nstatement S;\n@@\n- S\n-  foo();\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_EQ(2u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("S", P.Rules[0].Minus[0].Text);
  EXPECT_EQ("foo();", P.Rules[0].Minus[1].Text);
}

TEST(SmplParser, ALineHoldingDotsIsNeverJoined) {
  // `- if (x)` followed by `- { ... return ...; }` must stay two items. The
  // ellipsis is a path operator, and joining it moves the item away from the
  // refusal that names it.
  SemanticPatch P =
      parsed("@r@\nexpression x;\n@@\n- if (x)\n- { ... return ...; }\n");
  ASSERT_EQ(1u, P.Rules.size());
  ASSERT_LE(2u, P.Rules[0].Minus.size()) << refusalList(P);
  EXPECT_EQ("if (x)", P.Rules[0].Minus[0].Text);
}

} // namespace clang::spatch

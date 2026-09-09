//===--- ClangSpatch.cpp - Apply a semantic patch using Clang ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reads a semantic patch written in a subset of Coccinelle's SmPL and runs it
// against C or C++ translation units using Clang's own AST and control-flow
// graph.
//
// The tool reports three things and never conflates them: what it found, what
// it could not run, and what in the patch it did not understand. A semantic
// patch that analysed nothing looks exactly like one that found no problems, so
// the counts are always printed.
//
//===----------------------------------------------------------------------===//

#include "../PatchRunner.h"
#include "../PatternCompiler.h"
#include "../PatternParser.h"
#include "../SmplParser.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::spatch;
using namespace clang::tooling;

static llvm::cl::OptionCategory SpatchCategory("clang-spatch options");

static llvm::cl::opt<std::string>
    SpFile("sp-file",
           llvm::cl::desc("The semantic patch to apply, a .cocci file"),
           llvm::cl::value_desc("filename"), llvm::cl::Required,
           llvm::cl::cat(SpatchCategory));

static llvm::cl::opt<bool> AllowPartial(
    "allow-partial",
    llvm::cl::desc("Run the rules that compiled even though the patch contains "
                   "constructs this tool does not support. Off by default, "
                   "because a partly applied patch is unsafe"),
    llvm::cl::init(false), llvm::cl::cat(SpatchCategory));

static llvm::cl::opt<bool> PrintRewrite(
    "print-rewrite",
    llvm::cl::desc("Apply the patch and print each rewritten file whole, so "
                   "the result can be compared against a reference output "
                   "byte for byte"),
    llvm::cl::init(false), llvm::cl::cat(SpatchCategory));

static llvm::cl::opt<bool> PrintPatterns(
    "print-patterns",
    llvm::cl::desc("Parse each rule's patterns with Clang and print what each "
                   "statement became, then exit"),
    llvm::cl::init(false), llvm::cl::cat(SpatchCategory));

static llvm::cl::opt<bool>
    PrintMatchers("print-matchers",
                  llvm::cl::desc("Print the Clang matcher source each pattern "
                                 "compiled to, then exit"),
                  llvm::cl::init(false), llvm::cl::cat(SpatchCategory));

namespace {

/// Runs the patch over one translation unit.
class SpatchAction : public ASTFrontendAction {
public:
  SpatchAction(const SemanticPatch &Patch, RunResult &Result)
      : Patch(Patch), Result(Result) {}

  void EndSourceFileAction() override {
    runPatch(Patch, getCompilerInstance().getASTContext(), Result);
  }

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                 llvm::StringRef) override {
    return std::make_unique<ASTConsumer>();
  }

private:
  const SemanticPatch &Patch;
  RunResult &Result;
};

class SpatchActionFactory : public FrontendActionFactory {
public:
  SpatchActionFactory(const SemanticPatch &Patch, RunResult &Result)
      : Patch(Patch), Result(Result) {}

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<SpatchAction>(Patch, Result);
  }

private:
  const SemanticPatch &Patch;
  RunResult &Result;
};

} // namespace

int main(int argc, const char **argv) {
  llvm::Expected<CommonOptionsParser> Options =
      CommonOptionsParser::create(argc, argv, SpatchCategory);
  if (!Options) {
    llvm::errs() << llvm::toString(Options.takeError());
    return 1;
  }

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buffer =
      llvm::MemoryBuffer::getFile(SpFile);
  if (!Buffer) {
    llvm::errs() << "clang-spatch: cannot read " << SpFile << ": "
                 << Buffer.getError().message() << "\n";
    return 1;
  }

  std::string Error;
  std::optional<SemanticPatch> Patch =
      parseSemanticPatch((*Buffer)->getBuffer(), SpFile, Error);
  if (!Patch) {
    llvm::errs() << "clang-spatch: " << SpFile << ": " << Error << "\n";
    return 1;
  }

  // Print what was not understood before anything else, so it cannot be lost
  // below a wall of findings.
  for (const Refusal &R : Patch->Refusals)
    llvm::errs() << SpFile << ":" << R.Line << ": unsupported: " << R.Construct
                 << ": " << R.Reason << "\n";

  if (!Patch->fullyUnderstood() && !AllowPartial) {
    llvm::errs() << "clang-spatch: " << Patch->Refusals.size()
                 << " unsupported construct(s) in " << SpFile
                 << ", so no rule was run. Pass --allow-partial to run the "
                    "rules that did compile.\n";
    return 2;
  }

  if (PrintPatterns) {
    for (const Rule &R : Patch->Rules) {
      std::vector<std::string> Stmts;
      for (const PatternItem &I : R.Body)
        if (I.Kind == PatternItem::Kind::Statement)
          Stmts.push_back(I.Text);
      llvm::outs() << "rule " << (R.Name.empty() ? "<unnamed>" : R.Name)
                   << " statements=" << Stmts.size() << "\n";
      if (Stmts.empty())
        continue;
      std::string Error;
      std::optional<ParsedPattern> P = parsePattern(R.MetaVars, Stmts, Error);
      if (!P) {
        llvm::outs() << "  SYNTH FAILED: " << Error << "\n";
        continue;
      }
      for (unsigned I = 0; I != Stmts.size(); ++I)
        llvm::outs() << "  [" << Stmts[I] << "] -> "
                     << (P->Items[I] ? P->Items[I]->getStmtClassName()
                                     : "UNPARSED: " + P->Errors[I])
                     << "\n";
    }
    return 0;
  }

  if (PrintMatchers) {
    llvm::outs() << SpFile << ": " << Patch->Virtuals.size() << " virtual(s), "
                 << Patch->Rules.size() << " rule(s), "
                 << Patch->ScriptRules.size() << " script rule(s), "
                 << Patch->Refusals.size() << " refusal(s)\n";
    for (const Rule &R : Patch->Rules) {
      llvm::outs() << "rule " << (R.Name.empty() ? "<unnamed>" : R.Name)
                   << " quantifier="
                   << (!R.Quant                               ? "none"
                       : *R.Quant == Rule::Quantifier::Exists ? "exists"
                                                              : "forall")
                   << " metavars=" << R.MetaVars.size()
                   << " items=" << R.Body.size() << "\n";
      for (const MetaVar &M : R.MetaVars)
        llvm::outs() << "  metavar " << M.Name << "\n";
      for (const PatternItem &I : R.Body) {
        if (I.Kind == PatternItem::Kind::Dots) {
          llvm::outs() << "  dots when!=" << I.WhenNot.size();
          for (const std::string &W : I.WhenNot)
            llvm::outs() << " [" << W << "]";
          llvm::outs() << "\n";
          continue;
        }
        if (I.Kind == PatternItem::Kind::Disjunction) {
          llvm::outs() << "  disjunction branches=" << I.Branches.size()
                       << "\n";
          continue;
        }
        llvm::outs() << "  stmt [" << I.Text << "]\n";
        std::string CErr;
        if (std::optional<CompiledPattern> C =
                compileCallPattern(I.Text, R.MetaVars, CErr))
          llvm::outs() << "    matcher: " << C->MatcherSource << "\n";
        else
          llvm::outs() << "    NOT COMPILED: " << CErr << "\n";
      }
    }
    return 0;
  }

  RunResult Result;
  ClangTool Tool(Options->getCompilations(), Options->getSourcePathList());
  SpatchActionFactory Factory(*Patch, Result);
  const int ToolResult = Tool.run(&Factory);

  for (const Finding &F : Result.Findings)
    if (!PrintRewrite)
      llvm::outs() << F.File << ":" << F.Line << ":" << F.Col << ": "
                   << (F.RuleName.empty() ? "<unnamed>" : F.RuleName) << ": "
                   << F.Message << "\n";

  if (PrintRewrite) {
    // The whole file is printed rather than a diff. There is no unified-diff
    // printer in the LLVM tree, and a reference output is a whole file.
    // Every input is printed, edited or not. A patch that matches nothing
    // leaves the file unchanged, and that is the reference output for it, so
    // printing nothing would fail a comparison the tool actually passed.
    for (const std::string &Path : Options->getSourcePathList()) {
      llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Original =
          llvm::MemoryBuffer::getFile(Path);
      if (!Original) {
        llvm::errs() << "clang-spatch: cannot re-read " << Path << ": "
                     << Original.getError().message() << "\n";
        return 5;
      }
      auto Found = Result.Edits.find(Path);
      if (Found == Result.Edits.end()) {
        llvm::outs() << (*Original)->getBuffer();
        continue;
      }
      llvm::Expected<std::string> Rewritten = tooling::applyAllReplacements(
          (*Original)->getBuffer(), Found->second);
      if (!Rewritten) {
        llvm::errs() << "clang-spatch: " << Path << ": the edits do not apply: "
                     << llvm::toString(Rewritten.takeError()) << "\n";
        return 5;
      }
      llvm::outs() << *Rewritten;
    }
  }

  for (const Unrun &U : Result.UnrunRules)
    llvm::errs() << "clang-spatch: rule "
                 << (U.RuleName.empty() ? "<unnamed>" : U.RuleName)
                 << " was not run: " << U.Reason << "\n";

  llvm::errs() << "clang-spatch: " << Result.Findings.size() << " finding(s), "
               << Result.FunctionsAnalysed << " function(s) analysed, "
               << Result.FunctionsSkipped << " skipped, "
               << Result.AnchorsUnlocated << " unlocated, "
               << Result.AnchorsUnsupportedResource
               << " with an unsupported resource, "
               << Result.AnchorsUnattributed << " unattributed, "
               << Result.UnrunRules.size() << " rule(s) not run\n";

  // A rule that could not run, or an anchor that got no answer, is a failure
  // rather than a quiet zero.
  if (!Result.UnrunRules.empty())
    return 3;
  if (!Result.complete())
    return 4;
  return ToolResult == 0 ? 0 : 1;
}

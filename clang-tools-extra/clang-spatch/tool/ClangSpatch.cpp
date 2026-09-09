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
#include "../PatternParser.h"
#include "../SmplParser.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
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

/// Every pattern statement of one grouped side, branches included.
///
/// A branch's statements are pattern statements of the rule, so a printer that
/// stops at the top level reports `statements=0` for a rule whose whole body
/// is a disjunction and hides it from any sweep over this output.
void collectStatements(const std::vector<PatternItem> &Side,
                       std::vector<std::string> &Out) {
  for (const PatternItem &I : Side) {
    if (I.Kind == PatternItem::Kind::Disjunction) {
      for (const std::vector<PatternItem> &Branch : I.Branches)
        collectStatements(Branch, Out);
      continue;
    }
    if (I.Kind == PatternItem::Kind::Statement)
      Out.push_back(I.Text);
  }
}

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
      // Both sides are printed, because a context line is grouped into a
      // statement on each and the two statements can parse differently.
      for (const auto &[SideName, Side] :
           {std::pair{"minus", &R.Minus}, std::pair{"plus", &R.Plus}}) {
        std::vector<std::string> Stmts;
        collectStatements(*Side, Stmts);
        llvm::outs() << "rule " << (R.Name.empty() ? "<unnamed>" : R.Name)
                     << " side=" << SideName << " statements=" << Stmts.size()
                     << "\n";
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
    // A path is matched by identity rather than by spelling. A Replacement
    // names the file the way the compiler saw it, which is not the spelling on
    // the command line: given a relative path, every edit was dropped and the
    // input printed back unchanged, with the findings still reported and a
    // zero exit status.
    llvm::StringSet<> Consumed;
    for (const std::string &Path : Options->getSourcePathList()) {
      llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Original =
          llvm::MemoryBuffer::getFile(Path);
      if (!Original) {
        llvm::errs() << "clang-spatch: cannot re-read " << Path << ": "
                     << Original.getError().message() << "\n";
        return 5;
      }
      auto Found = Result.Edits.find(Path);
      if (Found == Result.Edits.end())
        for (auto It = Result.Edits.begin(), End = Result.Edits.end();
             It != End; ++It)
          if (llvm::sys::fs::equivalent(It->first(), Path)) {
            Found = It;
            break;
          }
      if (Found == Result.Edits.end()) {
        llvm::outs() << (*Original)->getBuffer();
        continue;
      }
      Consumed.insert(Found->first());
      llvm::Expected<std::string> Rewritten = tooling::applyAllReplacements(
          (*Original)->getBuffer(), Found->second);
      if (!Rewritten) {
        llvm::errs() << "clang-spatch: " << Path << ": the edits do not apply: "
                     << llvm::toString(Rewritten.takeError()) << "\n";
        return 5;
      }
      llvm::outs() << *Rewritten;
    }
    // Edits for a file nobody printed are a rewrite that was asked for and
    // silently lost, which must not read as success.
    for (const auto &File : Result.Edits)
      if (!Consumed.contains(File.first())) {
        llvm::errs() << "clang-spatch: " << File.first()
                     << ": edits were built for a file that is not in the "
                        "source path list, so the rewrite is incomplete\n";
        return 5;
      }
  }

  for (const Unrun &U : Result.UnreadBranches)
    llvm::errs() << "clang-spatch: rule "
                 << (U.RuleName.empty() ? "<unnamed>" : U.RuleName)
                 << " ran in part: " << U.Reason << "\n";

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
               << Result.UnreadBranches.size() << " branch(es) not read, "
               << Result.UnrunRules.size() << " rule(s) not run\n";

  // A rule that could not run, or an anchor that got no answer, is a failure
  // rather than a quiet zero.
  if (!Result.UnrunRules.empty())
    return 3;
  if (!Result.complete())
    return 4;
  return ToolResult == 0 ? 0 : 1;
}

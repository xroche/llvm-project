//===--- PatchRunner.h - Apply a semantic patch to a TU ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATCHRUNNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATCHRUNNER_H

#include "SemanticPatch.h"
#include "clang/AST/ASTContext.h"
#include <string>
#include <vector>

namespace clang::spatch {

/// One place a rule matched, reported the way Coccinelle reports a position so
/// the two tools' output can be compared directly.
struct Finding {
  std::string File;
  unsigned Line = 0;
  unsigned Col = 0;
  std::string RuleName;
  std::string Message;
};

/// A rule that could not be run, as distinct from one that ran and found
/// nothing.
struct Unrun {
  std::string RuleName;
  std::string Reason;
};

struct RunResult {
  std::vector<Finding> Findings;
  std::vector<Unrun> UnrunRules;
  /// Functions whose control-flow graph could not be built.
  unsigned FunctionsSkipped = 0;
  /// Anchors that matched but that the graph gives no program point, so the
  /// path property could not be asked about them.
  unsigned AnchorsUnlocated = 0;
  /// Anchors dropped because the resource they name is not a form this tool can
  /// compare. Only a plain reference to a declaration is, so `mutex_lock(l)`
  /// counts as a resource and `mutex_lock(&obj->lock)` does not.
  unsigned AnchorsUnsupportedResource = 0;
  /// Anchors whose enclosing function could not be found, or that have no
  /// usable source location, so no report could be attributed to them.
  unsigned AnchorsUnattributed = 0;
  unsigned FunctionsAnalysed = 0;

  /// Did every anchor that matched get an answer?
  ///
  /// False means the output is incomplete, and a caller must treat it as a
  /// failure rather than as an absence of findings.
  bool complete() const {
    return AnchorsUnlocated == 0 && AnchorsUnsupportedResource == 0 &&
           AnchorsUnattributed == 0 && FunctionsSkipped == 0;
  }
};

/// Runs every rule of \p Patch over the translation unit in \p Context.
void runPatch(const SemanticPatch &Patch, ASTContext &Context,
              RunResult &Result);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATCHRUNNER_H

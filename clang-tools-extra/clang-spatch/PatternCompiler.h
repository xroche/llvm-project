//===--- PatternCompiler.h - SmPL pattern to AST matcher --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compiles one statement of a semantic patch into a Clang AST matcher.
//
// The compilation goes through Clang's own textual matcher language rather than
// through a hand-written unifier between SmPL terms and AST nodes, because
// Clang already ships a parser for matcher text.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATTERNCOMPILER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATTERNCOMPILER_H

#include "SemanticPatch.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "llvm/ADT/ArrayRef.h"
#include <optional>
#include <string>
#include <vector>

namespace clang::spatch {

/// A pattern statement compiled to something that can be run against an AST.
struct CompiledPattern {
  ast_matchers::internal::DynTypedMatcher Matcher;
  /// The matcher source the compiler emitted. Kept because a user debugging a
  /// rule needs to see what their pattern became, and because it is the only
  /// way to tell a pattern bug from a matcher bug.
  std::string MatcherSource;
  /// The callee name the pattern names.
  std::string FunctionName;
  /// Metavariables bound by this pattern, in argument order. The names double
  /// as the matcher's binding ids, so a `when !=` clause naming the same
  /// metavariable can be tied to the anchor's binding.
  ///
  /// The matched call itself is bound under the id `root`.
  std::vector<std::string> Bindings;
};

/// Compiles a call-statement pattern such as `mutex_lock(l);`.
///
/// Returns std::nullopt and sets \p Error, naming the construct, for any shape
/// outside the supported subset.
std::optional<CompiledPattern>
compileCallPattern(llvm::StringRef Text, llvm::ArrayRef<MetaVar> MetaVars,
                   std::string &Error);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATTERNCOMPILER_H

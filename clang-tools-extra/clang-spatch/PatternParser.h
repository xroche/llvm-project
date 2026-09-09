//===--- PatternParser.h - Parse an SmPL pattern with Clang -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Turns the statements of one rule into Clang ASTs, by declaring the rule's
// metavariables and handing the result to Clang's own parser.
//
// The alternative is to emit Clang matcher source for each pattern, which is
// what this tool did first. That approach takes one statement form, because
// every new form needs new source to be generated for it, and the corpus
// spreads its demand across expressions, declarations, assignments and
// statements about evenly. Parsing the pattern instead covers every form Clang
// can read, and reduces matching to comparing two ASTs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATTERNPARSER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATTERNPARSER_H

#include "SemanticPatch.h"
#include "clang/Frontend/ASTUnit.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clang::spatch {

/// The name a caller can recognise an argument-level `...` by. It reaches the
/// AST as a call to this function, because `...` is not C and Clang has to
/// parse the pattern for anything else here to work.
constexpr llvm::StringLiteral DotsMarker = "__spatch_dots";

/// One rule's statements, parsed into ASTs that share one translation unit.
struct ParsedPattern {
  /// Owns every node reachable from \c Items, so it must outlive them.
  std::unique_ptr<ASTUnit> Unit;
  /// One entry per statement handed in, in the same order. Null where that
  /// statement did not parse, and \c Errors then names it.
  std::vector<const Stmt *> Items;
  /// The declaration each metavariable was given, so that a reference to one
  /// is recognised by comparing declarations rather than by comparing names.
  llvm::DenseMap<const Decl *, const MetaVar *> MetaVarDecls;
  /// The source handed to Clang. Kept because a reader debugging a rule needs
  /// to see what their pattern became, and because it separates a pattern bug
  /// from a synthesis bug.
  std::string Source;
  /// One message per statement that did not parse, in \c Items order.
  std::vector<std::string> Errors;

  /// Is \p D one of the rule's metavariables?
  const MetaVar *metaVarFor(const Decl *D) const {
    auto It = MetaVarDecls.find(D);
    return It == MetaVarDecls.end() ? nullptr : It->second;
  }
};

/// Parses \p Statements as C, with \p MetaVars declared so that a reference to
/// one resolves to a declaration this pattern owns.
///
/// Returns std::nullopt only when the synthesis itself cannot proceed, and
/// sets \p Error. A statement that fails to parse is reported per statement in
/// ParsedPattern::Errors rather than failing the whole rule, because one
/// unsupported line must not decide the fate of the others.
std::optional<ParsedPattern>
parsePattern(llvm::ArrayRef<MetaVar> MetaVars,
             llvm::ArrayRef<std::string> Statements, std::string &Error);

/// The declarations \p MetaVars need in order for \p Statements to parse.
///
/// Exposed for testing, because the type a metavariable is given is decided by
/// how the pattern uses it and that inference is the part most likely to be
/// wrong. An `expression x` used as `x->y` cannot be an `int`.
std::string synthesiseDeclarations(llvm::ArrayRef<MetaVar> MetaVars,
                                   llvm::ArrayRef<std::string> Statements);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATTERNPARSER_H

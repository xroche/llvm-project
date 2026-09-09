//===--- Unify.h - Match a pattern AST against a target AST -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compares two Clang ASTs, one parsed from a semantic patch and one from the
// code under it, treating a reference to a metavariable as a wildcard.
//
// This replaces generating Clang matcher source per pattern. That approach
// took one statement form, a bare call, and every new form needed new source
// generated for it: 620 pattern sites in the sample corpus report `only a call
// statement is supported as an anchor`. Comparing two trees costs the same for
// every form Clang can parse.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_UNIFY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_UNIFY_H

#include "PatternParser.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/StringMap.h"
#include <string>
#include <vector>

namespace clang::spatch {

/// What a match bound each metavariable to, in the target's tree.
using Bindings = llvm::StringMap<const Stmt *>;

/// One place a pattern matched.
struct Match {
  const Stmt *Node = nullptr; ///< The target subtree the pattern matched.
  Bindings Bound;
};

/// Does \p Pattern match \p Target, and if so what does it bind?
///
/// \p Parsed says which declarations are metavariables, so a reference to one
/// is recognised by comparing declarations rather than names. A metavariable
/// that appears twice must bind the same thing both times.
///
/// Both trees are compared through parentheses and implicit casts, because the
/// pattern is written without them and the target carries whatever the type
/// rules inserted.
bool unify(const Stmt *Pattern, const Stmt *Target, const ParsedPattern &Parsed,
           ASTContext &Context, Bindings &Bound);

/// Every place \p Pattern matches inside \p Context's translation unit.
///
/// Reported outermost first, and a match's subtrees are not searched again, so
/// a pattern that matches a call does not also report the same call reached
/// through its own argument.
std::vector<Match> findMatches(const Stmt *Pattern, const ParsedPattern &Parsed,
                               ASTContext &Context);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_UNIFY_H

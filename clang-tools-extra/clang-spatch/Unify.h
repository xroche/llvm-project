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

/// What a match bound one metavariable to.
///
/// A metavariable does not always stand for a subtree. `identifier x` in
/// `T x;` names the declarator and `type T` names the written type, and
/// neither of those has a `Stmt` of its own. So the range is what every
/// consumer shares, and the node is there for the ones that need the
/// declaration a reference names.
struct Binding {
  /// The subtree bound, or null when what was bound is not one.
  const Stmt *Node = nullptr;
  /// Where the bound text is, in the matched tree's source manager.
  SourceRange Range;
};

/// What a match bound each metavariable to, in the target's tree.
using Bindings = llvm::StringMap<Binding>;

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

/// Why the unifier cannot compare \p Pattern, as one sentence, or an empty
/// string when it can compare all of it.
///
/// Most node classes are decided by their class plus their children in order,
/// and that is unsound for a class whose identity also lies somewhere that is
/// not a child: a `goto` carries its label there, a cast its target type. A
/// declaration is compared field by field for that reason, and the classes of
/// type it can compare are limited in the same way. Every class the
/// comparison has been reasoned about is listed in the implementation, and
/// anything else is named here rather than approximated.
std::string whyNotComparable(const Stmt *Pattern);

/// Every place \p Pattern matches inside \p Context's translation unit.
///
/// Reported outermost first, and a match's subtrees are not searched again, so
/// a pattern that matches a call does not also report the same call reached
/// through its own argument.
std::vector<Match> findMatches(const Stmt *Pattern, const ParsedPattern &Parsed,
                               ASTContext &Context);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_UNIFY_H

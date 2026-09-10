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
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/TypeLoc.h"
#include "llvm/ADT/ArrayRef.h"
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

/// One pattern node and the target node it matched.
///
/// \c Pattern points into the pattern's own translation unit and \c Target
/// into the code's, so a holder must keep both alive. Recorded before
/// parentheses and implicit casts are peeled off, so a pattern written
/// `(i = i2)` maps to the target's own parentheses and an edit on it takes
/// them too.
struct NodePair {
  const Stmt *Pattern;
  const Stmt *Target;
};

/// Which target node each pattern node matched, in the order the comparison
/// reached them.
///
/// An edit inside the matched node needs this, because the `-` lines name a
/// sub-node of the pattern and the range to overwrite is the one it matched.
/// A token-sequence diff gets that wrong: it aligns `g(e, x)`'s `- x` with
/// `g(x, x)`'s first argument where `spatch` rewrites the second. Declarations
/// are absent, because a declarator is not a \c Stmt.
using NodePairs = std::vector<NodePair>;

/// One type occurrence written in the pattern and the one it matched.
///
/// A type is not a \c Stmt, so a rule marking the `int` of `T (*x[2])(int x)`
/// has no entry in \c NodePairs at all and the range to overwrite is a
/// \c TypeLoc's. Both sides point into their own translation unit, as in
/// \c NodePair.
struct TypeLocPair {
  TypeLoc Pattern;
  TypeLoc Target;
};

/// Which target type occurrence each written type of the pattern matched, in
/// the order the comparison reached them.
using TypeLocPairs = std::vector<TypeLocPair>;

/// One place a pattern matched.
struct Match {
  /// The target node the pattern matched.
  ///
  /// Not narrowed to a `Stmt`, because a declaration pattern also matches a
  /// declaration written outside any function body, and such a declaration
  /// is not one: there is no `DeclStmt` at file scope.
  DynTypedNode Node;
  Bindings Bound;
  /// Which of the patterns handed to \c findMatches matched here. Zero for a
  /// search over a single pattern.
  unsigned Pattern = 0;
  /// Filled only when \c MatchOptions::WantNodePairs asked for it.
  NodePairs Pairs;
  /// Filled only when \c MatchOptions::WantNodePairs asked for it.
  TypeLocPairs TypePairs;
};

/// What a search over the target is allowed to match.
struct MatchOptions {
  /// Metavariable values the match must agree with, or null for none.
  ///
  /// This is how an inherited declaration such as `expression r.X` is
  /// honoured: the value an earlier rule bound is seeded here, and the
  /// unifier's own consistency check rejects every site that disagrees with
  /// it. Without a seed the declaration matches anything, which rewrites more
  /// than the patch asked for.
  const Bindings *Inherited = nullptr;
  /// Also match a file-scope declaration that declares more than one thing.
  ///
  /// Such a declaration cannot be rewritten, because its declarators share
  /// one `;` and replacing one would take the terminator the others need, so
  /// only a rule that asks for no change may be given it.
  bool MultiDeclaratorOK = false;
  /// Record \c Match::Pairs and \c Match::TypePairs for every match.
  ///
  /// Off by default, because a rule that replaces a whole statement has no use
  /// for it and every match would carry the vector.
  bool WantNodePairs = false;
  /// Which of the patterns handed in may also match inside a site they
  /// already matched, one entry per pattern in the same order, or empty for
  /// none of them.
  ///
  /// Coccinelle excludes no subtree of a match. A pattern written as a whole
  /// statement matches at statement positions only, which is a restriction
  /// this search cannot express, so the exclusion stands in for it. The
  /// terminator that tells the two shapes apart is stripped before the
  /// pattern is parsed, so the caller has to say which was written. An
  /// earlier pattern still wins over a later one for the same text, because
  /// that is branch order.
  llvm::ArrayRef<bool> MayNest;
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
                               ASTContext &Context, MatchOptions Opts = {});

/// Every place any of \p Patterns matches, with an earlier pattern winning
/// over a later one wherever the two want the same text.
///
/// This is what a disjunction means. Each branch is searched over the whole
/// translation unit before the next one is, and a branch may not take text an
/// earlier branch already took, so branch order beats nesting. Matches are
/// reported in source order.
std::vector<Match> findMatches(llvm::ArrayRef<const Stmt *> Patterns,
                               const ParsedPattern &Parsed, ASTContext &Context,
                               MatchOptions Opts = {});

/// A string that tells two bindings apart exactly as the unifier's own
/// consistency check does.
///
/// Exposed so that a caller passing environments from one rule to the next can
/// drop the duplicates among them without comparing every pair.
std::string bindingKey(const Binding &B, ASTContext &Context);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_UNIFY_H

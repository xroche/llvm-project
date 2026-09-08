//===--- PathQuery.h - Control-flow path queries ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Answers the question a statement-level `...` asks in a semantic patch: on the
// paths between two program points, does some forbidden construct occur?
//
// Both quantifiers reduce to reachability over the control-flow graph, with no
// fixpoint. The semantics were fixed by measuring Coccinelle's own verdicts on
// a five-case corpus, and two of them are not what a first reading suggests.
// `Forall` is a property of the whole function rather than of one exit, because
// a per-exit reading reports a function that releases on another branch, which
// Coccinelle does not. And a construct the function can never reach counts as
// absent, which is what separates a control-flow answer from a syntactic one.
//
// Reaching the exit is not the same as returning. Clang ties a `noreturn` call
// such as `abort()` to the exit block, so a path that terminates the process
// counts as a path to the exit here. Coccinelle's `when strict` is the operator
// that distinguishes the two, and this file does not implement it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATHQUERY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATHQUERY_H

#include "clang/Analysis/CFG.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include <functional>

namespace clang::spatch {

/// A position in the control-flow graph: element \c Elem of the block with id
/// \c Block. An \c Elem equal to the block's size denotes the position after
/// its last element, whose successors are the successor blocks' first
/// positions.
///
/// Element granularity is required because an anchor and a forbidden construct
/// frequently land in the same basic block.
struct Point {
  unsigned Block = 0;
  unsigned Elem = 0;
};

/// Decides whether the statement at a program point is the construct a `when
/// !=` clause forbids. Returning true for a point makes it forbidden.
///
/// Must be a pure function of the point. The walk decides each point once and
/// caches that decision, so a predicate whose answer depends on the route
/// taken to reach the point would give a result that depends on visit order.
using StmtPredicate = std::function<bool(const Stmt *)>;

/// Indexes a CFG by block id so a walk can follow ids rather than pointers.
class CFGIndex {
public:
  explicit CFGIndex(CFG &G);

  CFG &graph() const { return *G; }
  unsigned exitBlock() const { return G->getExit().getBlockID(); }
  /// Null when the id belongs to no reachable block.
  CFGBlock *block(unsigned ID) const {
    return ID < ByID.size() ? ByID[ID] : nullptr;
  }
  /// The statement at \p P, or null when that position holds anything else.
  const Stmt *stmtAt(Point P) const;

private:
  CFG *G;
  llvm::SmallVector<CFGBlock *, 32> ByID;
};

/// Is there a path from \p From to the function's exit along which no point
/// satisfies \p Forbidden? This is the `exists` quantifier.
bool somePathAvoids(const CFGIndex &Index, Point From,
                    const StmtPredicate &Forbidden);

/// Does no point satisfying \p Forbidden lie on any path from \p From to the
/// function's exit? This is the `forall` quantifier.
///
/// A forbidden point counts only when it is both reachable from \p From and
/// able to reach the exit, so a construct stranded behind a `goto` does not
/// suppress the answer.
bool noPathReaches(const CFGIndex &Index, Point From,
                   const StmtPredicate &Forbidden);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_PATHQUERY_H

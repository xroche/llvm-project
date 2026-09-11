//===--- PathQuery.cpp - Control-flow path queries ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PathQuery.h"
#include "llvm/ADT/DenseSet.h"

namespace clang::spatch {

namespace {

uint64_t pointKey(Point P) {
  return (static_cast<uint64_t>(P.Block) << 32) | P.Elem;
}

/// Walks forward from \p Start over program points.
///
/// With \p StopAtForbidden a forbidden point is not entered, so reaching the
/// exit means some path avoids it. Without it the walk is plain reachability,
/// and every forbidden point entered is appended to \p Found.
///
/// A successor edge whose reachable target is null is still an edge. Clang
/// nulls it and keeps the block on the side, reachable through
/// `AdjacentBlock::getPossiblyUnreachableBlock`, so `for (;;)` gives its header
/// `Succs (2): B2 NULL` with the code after the loop still there. Coccinelle
/// keeps the same edge as a synthetic `[forfall]` node and walks into whatever
/// follows the loop even when no execution gets there, so the walk follows the
/// unreachable block too and only treats the edge as leaving the function when
/// there is no block behind it at all.
///
/// Skipping these edges made `exists` miss a lock held across a
/// non-terminating loop. Treating them all as the exit instead reported a lock
/// that is released by dead code after such a loop, which Coccinelle does not.
bool walk(const CFGIndex &Index, Point Start, const StmtPredicate &Forbidden,
          bool StopAtForbidden, llvm::SmallVectorImpl<Point> *Found) {
  const unsigned ExitID = Index.exitBlock();
  llvm::DenseSet<uint64_t> Seen;
  llvm::SmallVector<Point, 32> Work;
  Work.push_back(Start);
  bool ReachedExit = false;

  while (!Work.empty()) {
    const Point P = Work.pop_back_val();
    if (!Seen.insert(pointKey(P)).second)
      continue;
    if (P.Block == ExitID) {
      ReachedExit = true;
      continue;
    }
    CFGBlock *B = Index.block(P.Block);
    if (!B)
      continue;

    if (P.Elem < B->size()) {
      if (const Stmt *S = Index.stmtAt(P))
        if (Forbidden(S)) {
          if (Found)
            Found->push_back(P);
          if (StopAtForbidden)
            continue;
        }
      Work.push_back({P.Block, P.Elem + 1});
      continue;
    }
    for (const CFGBlock::AdjacentBlock &A : B->succs()) {
      if (CFGBlock *S = A.getReachableBlock())
        Work.push_back({S->getBlockID(), 0});
      else if (CFGBlock *U = A.getPossiblyUnreachableBlock())
        Work.push_back({U->getBlockID(), 0});
      else
        ReachedExit = true;
    }
  }
  return ReachedExit;
}

} // namespace

CFGIndex::CFGIndex(CFG &G) : G(&G), ByID(G.getNumBlockIDs(), nullptr) {
  for (CFGBlock *B : G)
    ByID[B->getBlockID()] = B;
}

const Stmt *CFGIndex::stmtAt(Point P) const {
  CFGBlock *B = block(P.Block);
  if (!B || P.Elem >= B->size())
    return nullptr;
  std::optional<CFGStmt> S = (*B)[P.Elem].getAs<CFGStmt>();
  return S ? S->getStmt() : nullptr;
}

bool somePathAvoids(const CFGIndex &Index, Point From,
                    const StmtPredicate &Forbidden) {
  return walk(Index, From, Forbidden, /*StopAtForbidden=*/true, nullptr);
}

bool noPathReaches(const CFGIndex &Index, Point From,
                   const StmtPredicate &Forbidden) {
  llvm::SmallVector<Point, 8> Found;
  walk(Index, From, Forbidden, /*StopAtForbidden=*/false, &Found);
  // A forbidden point suppresses the answer only when it lies on a path from
  // From to the exit, so it must also be able to reach the exit itself.
  for (Point F : Found)
    if (walk(Index, F, Forbidden, /*StopAtForbidden=*/false, nullptr))
      return false;
  return true;
}

} // namespace clang::spatch

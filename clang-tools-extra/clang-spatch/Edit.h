//===--- Edit.h - Turn a matched pattern into source edits ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Builds the source edits a rule's `-` and `+` lines ask for at one match.
//
// A semantic patch is a patch, so a tool that only reports matches cannot be
// compared against the reference implementation on most of a real corpus: of
// 1209 native patches, 1067 change code and only 142 are pure match or report.
// The 768 patches that ship an expected output are unusable without this.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_EDIT_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_EDIT_H

#include "SemanticPatch.h"
#include "Unify.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace clang::spatch {

/// The replacement one match asks for: the range to overwrite and the text to
/// put there. An insertion has an empty range and a deletion empty text.
struct PatternEdit {
  tooling::Replacement Replacement;
};

/// Builds the edit that replaces \p Matched with the rule's `+` lines.
///
/// \p PlusText is the `+` lines already joined, with metavariable names
/// appearing verbatim. Each name is substituted with the source text of what
/// it bound to in \p Bound, so `+ bar(E)` against `foo(x + 1)` gives
/// `bar(x + 1)`.
///
/// Returns std::nullopt and sets \p Error when no edit can be built. That is
/// not a failure of the match, and a caller must count it rather than ignore
/// it: the commonest cause is a range inside a macro expansion, where
/// `Replacement::setFromSourceRange` would take the spelling location and
/// rewrite the macro's definition, changing every other expansion of it and
/// reporting success.
std::optional<PatternEdit> buildEdit(DynTypedNode Matched,
                                     llvm::StringRef PlusText,
                                     const Bindings &Bound, ASTContext &Context,
                                     std::string &Error);

/// The source text of \p S exactly as written, macros included.
///
/// A node's `getEndLoc()` is the start of its last token rather than its end,
/// so the range is extended to cover that token before the text is read.
llvm::StringRef sourceTextOf(const Stmt &S, ASTContext &Context);

/// The source text of \p Range exactly as written, its last token included.
llvm::StringRef sourceTextOf(SourceRange Range, ASTContext &Context);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_EDIT_H

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
/// \p PatternEndsInSemicolon says whether the `-` side was written as a whole
/// statement, terminator included. A pattern written as a bare expression
/// leaves the terminator where it is, so `- bar(F)` over `+ 4` rewrites
/// `bar(12);` to `4;` rather than to `4`.
///
/// Returns std::nullopt and sets \p Error when no edit can be built. That is
/// not a failure of the match, and a caller must count it rather than ignore
/// it: the commonest cause is a range inside a macro expansion, where
/// `Replacement::setFromSourceRange` would take the spelling location and
/// rewrite the macro's definition, changing every other expansion of it and
/// reporting success.
std::optional<PatternEdit> buildEdit(DynTypedNode Matched,
                                     llvm::StringRef PlusText,
                                     bool PatternEndsInSemicolon,
                                     const Bindings &Bound, ASTContext &Context,
                                     std::string &Error);

/// The target node the `-` lines of a partly changed statement correspond to,
/// or null when they cover no whole node of the pattern.
///
/// \p PatternBegin and \p PatternEnd are the characters the `-` lines occupy
/// in the pattern's own source, and \p Pairs says which target node each
/// pattern node matched.
///
/// Covering no node is common and is not an error. `- foo(` over `+ bar(`
/// marks a callee and a parenthesis, and `- -` over `  x` marks part of a
/// unary operator. Neither has a range of its own in the target, so a caller
/// falls back to replacing the whole match with the plus side reassembled.
const Stmt *innerEditTarget(unsigned PatternBegin, unsigned PatternEnd,
                            const NodePairs &Pairs,
                            ASTContext &PatternContext);

/// Builds the edit that writes \p PlusText over \p Target and leaves the rest
/// of the matched statement as the target wrote it.
///
/// This is the faithful shape, because Coccinelle removes the `-` tokens and
/// puts the `+` tokens where they stood while every context token keeps the
/// layout it already had. Replacing the whole match reprints that layout from
/// the pattern instead, which loses the target's own line breaks and spacing.
///
/// Returns std::nullopt and sets \p Error when the range cannot be edited,
/// which a caller must count rather than fall back on: a range inside a macro
/// expansion is refused for the same reason \c buildEdit refuses one.
std::optional<PatternEdit> buildInnerEdit(const Stmt &Target,
                                          llvm::StringRef PlusText,
                                          const Bindings &Bound,
                                          ASTContext &Context,
                                          std::string &Error);

/// The characters an in-place edit over \p Range overwrites, and the text it
/// writes there.
///
/// Coccinelle takes some of the whitespace around a region it rewrites and
/// leaves the rest. The rule is a fit to a probe matrix over `spatch` 1.1.1
/// that varies the token on each side, the whitespace on each side and the
/// number of target tokens in the region. Reading `unparse_c.ml` predicted
/// the wrong answer three times when the deletion rule was written, so this
/// was measured rather than read.
///
/// - The whitespace before the region goes when the token before it is `(`,
///   and stays otherwise. A `[` does not take it.
/// - The whitespace after the region stays when the token after it is a
///   binary operator, and one space is written when there was none.
/// - Otherwise it goes before a `,`, a `)` or a `;`, unless the region is a
///   single token, which keeps it.
/// - Otherwise it stays as written. Only those three followers were measured,
///   so any other one keeps what the target wrote.
///
/// \p Text is updated in place when a space has to be written.
CharSourceRange inPlaceEditRange(CharSourceRange Range, std::string &Text,
                                 ASTContext &Context);

/// The source text of \p S exactly as written, macros included.
///
/// A node's `getEndLoc()` is the start of its last token rather than its end,
/// so the range is extended to cover that token before the text is read.
llvm::StringRef sourceTextOf(const Stmt &S, ASTContext &Context);

/// The source text of \p Range exactly as written, its last token included.
llvm::StringRef sourceTextOf(SourceRange Range, ASTContext &Context);

/// Widens every pure deletion in \p Reps so that deleting a statement does not
/// leave its indentation, its line or a newly blank line behind.
///
/// \p Buffer is the contents of \p FilePath, which is the file every widened
/// replacement is built against. A replacement with text in it is returned
/// unchanged: only a deletion can leave whitespace behind.
///
/// Coccinelle deletes on a token stream whose whitespace is attached to the
/// tokens, so its output has no leftover line where a statement was. The rule
/// reproduced here was measured against `spatch` rather than read off its
/// source, which applies nine ordered passes over that stream:
///
/// - A deletion that covers whole lines takes those lines, newline included.
/// - Blank lines directly above it go too, once the deletion is followed by a
///   blank line or is the last thing in its block.
/// - A deletion that directly follows a `{` takes the blank lines below it
///   instead, so opening a block does not leave a gap at the top.
/// - A deletion that shares its line with kept code takes the horizontal
///   whitespace after it, and the whitespace before it as well when nothing
///   but the newline follows.
tooling::Replacements widenDeletions(llvm::StringRef FilePath,
                                     llvm::StringRef Buffer,
                                     const tooling::Replacements &Reps);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_EDIT_H

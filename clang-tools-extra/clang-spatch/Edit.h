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

/// What an in-place edit overwrites, and what kind of thing it is.
struct InnerEdit {
  /// Invalid when the `-` lines cover no whole node of the pattern.
  SourceRange Range;
  /// The range is a written type occurrence rather than a statement. The
  /// whitespace rule reads the two differently, because a `*` after a type
  /// declares a pointer and a `*` after an expression multiplies.
  bool IsAWrittenType = false;
};

/// The target text that the `-` lines of a partly changed statement
/// correspond to.
///
/// \p PatternBegin and \p PatternEnd are the characters the `-` lines occupy
/// in the pattern's own source. \p Pairs says which target node each pattern
/// node matched and \p TypePairs the same for each written type, which is
/// where a rule marking the `int` of `T (*x[2])(int x)` is answered: a type is
/// not a \c Stmt, so it has no entry in \p Pairs at all.
///
/// \p DeclarationPairs answers a rule marking a whole record member, which is
/// neither of those. A member's range stops before its `;` while the patch
/// marks the line including it, so each side of such a pair is taken through
/// its own terminator, in its own \p PatternContext or \p Context.
///
/// Covering no node is common and is not an error. `- foo(` over `+ bar(`
/// marks a callee and a parenthesis, and `- -` over `  x` marks part of a
/// unary operator. Neither has a range of its own in the target, so a caller
/// falls back to replacing the whole match with the plus side reassembled.
InnerEdit innerEditRange(unsigned PatternBegin, unsigned PatternEnd,
                         const NodePairs &Pairs, const TypeLocPairs &TypePairs,
                         const DeclarationPairs &DeclarationPairs,
                         ASTContext &PatternContext, ASTContext &Context);

/// Builds the edit that writes \p PlusText over the target range \p Target
/// and leaves the rest of the matched statement as the target wrote it.
///
/// This is the faithful shape, because Coccinelle removes the `-` tokens and
/// puts the `+` tokens where they stood while every context token keeps the
/// layout it already had. Replacing the whole match reprints that layout from
/// the pattern instead, which loses the target's own line breaks and spacing.
///
/// \p RangeIsAWrittenType is \c InnerEdit::IsAWrittenType, which the
/// whitespace rule reads.
///
/// Returns std::nullopt and sets \p Error when the range cannot be edited,
/// which a caller must count rather than fall back on: a range inside a macro
/// expansion is refused for the same reason \c buildEdit refuses one.
std::optional<PatternEdit>
buildInnerEdit(SourceRange Target, llvm::StringRef PlusText,
               bool RangeIsAWrittenType, const Bindings &Bound,
               ASTContext &Context, std::string &Error);

/// The characters an in-place edit over \p Range overwrites, and the text it
/// writes there.
///
/// Coccinelle takes some of the whitespace around a region it rewrites and
/// leaves the rest. Every clause below was measured against `spatch` 1.1.1
/// rather than read off its source, which is what stops a later reader
/// deriving a different rule and calling this one a bug.
///
/// - The whitespace before the region goes when the token before it is `(`,
///   and stays otherwise. A `[` does not take it.
/// - The whitespace after the region goes when \p Text ends in a pointer
///   star, which binds to whatever follows it.
/// - The whitespace after the region stays when the token after it is a
///   binary operator, and one space is written when there was none. A `*`
///   after a written type is a declarator star and not an operator, so
///   \p RangeIsAWrittenType turns that clause off and `LPINT*y` becomes
///   `unsigned*y`.
/// - Otherwise it goes before a `,`, a `)` or a `;`, unless the region is a
///   single token, which keeps it.
/// - Otherwise it stays as written. Only those three followers were measured,
///   so any other one keeps what the target wrote.
///
/// \p Text is updated in place when a space has to be written, so a caller
/// writes the returned range with the updated \p Text and not with the string
/// it passed in.
CharSourceRange inPlaceEditRange(CharSourceRange Range, std::string &Text,
                                 ASTContext &Context, bool RangeIsAWrittenType);

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

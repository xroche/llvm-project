//===--- SmplParser.h - Parser for a subset of SmPL -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A parser for the subset of Coccinelle's SmPL that this tool can compile.
// It depends on no Clang AST, so a .cocci file can be read and checked
// without a compiler instance.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_SMPLPARSER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_SMPLPARSER_H

#include "SemanticPatch.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace clang::spatch {

/// Parses \p Text, the contents of a .cocci file. Returns the patch with any
/// unsupported constructs recorded in its Refusals rather than dropped.
/// Returns std::nullopt only when the input is not a semantic patch at all.
///
/// The two outcomes split along what Coccinelle itself does with the input.
/// A construct Coccinelle accepts but this tool cannot compile becomes a
/// Refusal and parsing continues, so one call reports every such construct in
/// the file. A construct Coccinelle rejects sets \p Error and yields no patch,
/// because text that no Coccinelle would accept is not a semantic patch.
///
/// \p Filename appears in \p Error and nowhere else.
std::optional<SemanticPatch> parseSemanticPatch(llvm::StringRef Text,
                                                llvm::StringRef Filename,
                                                std::string &Error);

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_SMPLPARSER_H

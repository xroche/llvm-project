//===--- Edit.cpp - Turn a matched pattern into source edits -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Edit.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Transformer/SourceCode.h"
#include "llvm/ADT/StringExtras.h"

namespace clang::spatch {

namespace {

bool isIdentChar(char C) {
  return isalnum(static_cast<unsigned char>(C)) || C == '_';
}

/// Replaces every whole-word occurrence of each binding's name in \p Text with
/// the source text the binding holds.
///
/// Whole-word, because a metavariable named `E` must not be substituted inside
/// `END` or inside another metavariable's name.
std::string substitute(llvm::StringRef Text, const Bindings &Bound,
                       ASTContext &Context) {
  std::string Out = Text.str();
  for (const auto &Entry : Bound) {
    const std::string Name = Entry.first().str();
    const std::string Value = sourceTextOf(Entry.second.Range, Context).str();
    for (size_t At = 0; (At = Out.find(Name, At)) != std::string::npos;) {
      const bool LeftOK = At == 0 || !isIdentChar(Out[At - 1]);
      const size_t End = At + Name.size();
      const bool RightOK = End == Out.size() || !isIdentChar(Out[End]);
      if (!LeftOK || !RightOK) {
        At = End;
        continue;
      }
      Out.replace(At, Name.size(), Value);
      At += Value.size();
    }
  }
  return Out;
}

/// Does \p Matched own the semicolon that follows it?
///
/// A declaration does. A statement of a block does too, and C has no node for
/// an expression statement, so a call written as a statement is a `CallExpr`
/// whose parent is the enclosing `CompoundStmt`. That parent is what
/// separates it from the same `CallExpr` used as an operand: extending
/// unconditionally rewrote `return -1;` to `return 1`.
bool ownsItsTerminator(DynTypedNode Matched, ASTContext &Context) {
  if (Matched.get<Decl>())
    return true;
  const auto *S = Matched.get<Stmt>();
  if (!S)
    return false;
  for (const DynTypedNode &P : Context.getParents(*S))
    if (P.get<CompoundStmt>())
      return true;
  return false;
}

/// The range \p Matched occupies, extended over its terminating semicolon
/// when it owns one, so that replacing it does not leave the terminator
/// behind.
CharSourceRange matchedRange(DynTypedNode Matched, ASTContext &Context) {
  const CharSourceRange Token =
      CharSourceRange::getTokenRange(Matched.getSourceRange());
  if (!ownsItsTerminator(Matched, Context))
    return Token;
  return tooling::maybeExtendRange(Token, tok::semi, Context);
}

} // namespace

llvm::StringRef sourceTextOf(const Stmt &S, ASTContext &Context) {
  return sourceTextOf(S.getSourceRange(), Context);
}

llvm::StringRef sourceTextOf(SourceRange Range, ASTContext &Context) {
  return Lexer::getSourceText(CharSourceRange::getTokenRange(Range),
                              Context.getSourceManager(),
                              Context.getLangOpts());
}

std::optional<PatternEdit> buildEdit(DynTypedNode Matched,
                                     llvm::StringRef PlusText,
                                     const Bindings &Bound, ASTContext &Context,
                                     std::string &Error) {
  const CharSourceRange Range = matchedRange(Matched, Context);
  // Every range is validated before a Replacement is built from it, because
  // `Replacement::setFromSourceRange` takes the spelling location
  // unconditionally. A node spelled inside a macro body would otherwise get an
  // edit pointing at the definition, rewriting every other expansion.
  if (llvm::Error Invalid =
          tooling::validateEditRange(Range, Context.getSourceManager())) {
    Error = "the matched range cannot be edited: " +
            llvm::toString(std::move(Invalid));
    return std::nullopt;
  }

  const std::string Text = substitute(PlusText, Bound, Context);
  return PatternEdit{tooling::Replacement(Context.getSourceManager(), Range,
                                          Text, Context.getLangOpts())};
}

} // namespace clang::spatch

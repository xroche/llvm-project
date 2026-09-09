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
    const std::string Value = sourceTextOf(*Entry.second, Context).str();
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

/// Is \p S a statement of a block rather than a part of a larger statement?
///
/// C has no node for an expression statement, so a call written as a
/// statement is a `CallExpr` whose parent is the enclosing `CompoundStmt`.
/// That is what separates it from the same `CallExpr` used as an operand.
bool isStatementOfABlock(const Stmt &S, ASTContext &Context) {
  for (const DynTypedNode &P : Context.getParents(S))
    if (P.get<CompoundStmt>())
      return true;
  return false;
}

/// The range \p S occupies, extended over its terminating semicolon when it
/// is a statement of a block, so that replacing one does not leave the
/// terminator behind.
///
/// The semicolon belongs to the enclosing statement rather than to a part of
/// it, so extending unconditionally rewrote `return -1;` to `return 1` and
/// dropped the terminator.
CharSourceRange statementRange(const Stmt &S, ASTContext &Context) {
  const CharSourceRange Token =
      CharSourceRange::getTokenRange(S.getSourceRange());
  if (!isStatementOfABlock(S, Context))
    return Token;
  return tooling::maybeExtendRange(Token, tok::semi, Context);
}

} // namespace

llvm::StringRef sourceTextOf(const Stmt &S, ASTContext &Context) {
  return Lexer::getSourceText(
      CharSourceRange::getTokenRange(S.getSourceRange()),
      Context.getSourceManager(), Context.getLangOpts());
}

std::optional<PatternEdit> buildEdit(const Stmt &Matched,
                                     llvm::StringRef PlusText,
                                     const Bindings &Bound, ASTContext &Context,
                                     std::string &Error) {
  const CharSourceRange Range = statementRange(Matched, Context);
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

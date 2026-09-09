//===--- Edit.cpp - Turn a matched pattern into source edits -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Edit.h"
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
std::string substitute(llvm::StringRef Text,
                       llvm::ArrayRef<std::string> Bindings,
                       const ast_matchers::BoundNodes &Nodes,
                       ASTContext &Context) {
  std::string Out = Text.str();
  for (const std::string &Name : Bindings) {
    const auto *Bound = Nodes.getNodeAs<Stmt>(Name);
    if (!Bound)
      continue;
    const std::string Value = sourceTextOf(*Bound, Context).str();
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

/// The range \p S occupies, extended over the semicolon that ends it when the
/// statement has one, so that replacing a statement does not leave its
/// terminator behind.
CharSourceRange statementRange(const Stmt &S, ASTContext &Context) {
  const CharSourceRange Token =
      CharSourceRange::getTokenRange(S.getSourceRange());
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
                                     const ast_matchers::BoundNodes &Nodes,
                                     llvm::ArrayRef<std::string> Bindings,
                                     ASTContext &Context, std::string &Error) {
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

  const std::string Text = substitute(PlusText, Bindings, Nodes, Context);
  return PatternEdit{tooling::Replacement(Context.getSourceManager(), Range,
                                          Text, Context.getLangOpts())};
}

} // namespace clang::spatch

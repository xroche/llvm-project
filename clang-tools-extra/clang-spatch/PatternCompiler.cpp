//===--- PatternCompiler.cpp - SmPL pattern to AST matcher ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PatternCompiler.h"
#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace clang::spatch {

namespace {

/// Is \p S a C or C++ keyword? A pattern line such as `if (e)` looks like a
/// call to this compiler, and emitting a matcher for a function named `if`
/// would match nothing while reporting that the pattern compiled.
bool isKeyword(llvm::StringRef S) {
  static const char *const Keywords[] = {"if",
                                         "while",
                                         "for",
                                         "return",
                                         "sizeof",
                                         "switch",
                                         "case",
                                         "default",
                                         "goto",
                                         "do",
                                         "else",
                                         "break",
                                         "continue",
                                         "typeof",
                                         "alignof",
                                         "_Alignof",
                                         "typeid",
                                         "throw",
                                         "new",
                                         "delete",
                                         "noexcept",
                                         "static_cast",
                                         "dynamic_cast",
                                         "const_cast",
                                         "reinterpret_cast",
                                         "decltype"};
  return llvm::is_contained(Keywords, S);
}

bool isIdentifier(llvm::StringRef S) {
  if (S.empty() || !(isalpha(S[0]) || S[0] == '_'))
    return false;
  return llvm::all_of(S, [](char C) { return isalnum(C) || C == '_'; });
}

/// Splits an argument list on commas that are not nested inside brackets.
/// Returns false when the brackets do not balance.
bool splitArguments(llvm::StringRef Args,
                    llvm::SmallVectorImpl<llvm::StringRef> &Out) {
  int Depth = 0;
  size_t Start = 0;
  for (size_t I = 0; I != Args.size(); ++I) {
    const char C = Args[I];
    if (C == '(' || C == '[')
      ++Depth;
    else if (C == ')' || C == ']')
      --Depth;
    else if (C == ',' && Depth == 0) {
      Out.push_back(Args.substr(Start, I - Start).trim());
      Start = I + 1;
    }
    if (Depth < 0)
      return false;
  }
  if (Depth != 0)
    return false;
  llvm::StringRef Last = Args.substr(Start).trim();
  if (!Last.empty() || !Out.empty())
    Out.push_back(Last);
  return true;
}

const MetaVar *findMetaVar(llvm::ArrayRef<MetaVar> MetaVars,
                           llvm::StringRef Name) {
  for (const MetaVar &M : MetaVars)
    if (M.Name == Name)
      return &M;
  return nullptr;
}

/// The matcher source for one argument, or empty when the argument is outside
/// the subset.
std::string argumentMatcher(llvm::StringRef Arg,
                            llvm::ArrayRef<MetaVar> MetaVars,
                            std::string &Error) {
  if (const MetaVar *M = findMetaVar(MetaVars, Arg)) {
    switch (M->Kind) {
    case MetaVar::Kind::Expression:
      return "expr().bind(\"" + M->Name + "\")";
    case MetaVar::Kind::Constant:
      // Not `expr()`. A constant metavariable describes a literal or an
      // enumerator, and Coccinelle's own expected output for tests/constx.cocci
      // rewrites foo(12) and foo('a') while leaving foo(x) alone.
      return "expr(anyOf(integerLiteral(), floatLiteral(), stringLiteral(), "
             "characterLiteral(), cxxBoolLiteral(), "
             "declRefExpr(to(enumConstantDecl())))).bind(\"" +
             M->Name + "\")";
    case MetaVar::Kind::Identifier:
      return "declRefExpr(to(namedDecl())).bind(\"" + M->Name + "\")";
    case MetaVar::Kind::Statement:
      Error = "statement metavariable used as a call argument";
      return {};
    case MetaVar::Kind::Type:
      Error = "type metavariable `" + M->Name +
              "` used as a call argument, which needs a matcher over the "
              "argument's type rather than over its value";
      return {};
    case MetaVar::Kind::Position:
      Error = "position metavariable used as a call argument";
      return {};
    }
  }
  // An integer literal is the one non-metavariable argument the subset takes,
  // because several real rules pin a flag or a size that way.
  long long Value = 0;
  if (!Arg.empty() && !Arg.getAsInteger(0, Value))
    return ("integerLiteral(equals(" + llvm::Twine(Value) + "))").str();

  Error = ("argument `" + Arg +
           "` is neither a declared metavariable nor an integer literal")
              .str();
  return {};
}

} // namespace

std::optional<CompiledPattern>
compileCallPattern(llvm::StringRef Text, llvm::ArrayRef<MetaVar> MetaVars,
                   std::string &Error) {
  llvm::StringRef S = Text.trim();
  S.consume_back(";");
  S = S.trim();

  const size_t Open = S.find('(');
  if (Open == llvm::StringRef::npos || !S.ends_with(")")) {
    Error = "only a call statement is supported as an anchor";
    return std::nullopt;
  }
  const llvm::StringRef Name = S.substr(0, Open).trim();
  if (isKeyword(Name)) {
    Error = ("`" + Name +
             "` is a keyword rather than a callee, so this pattern line is a "
             "statement form outside the subset")
                .str();
    return std::nullopt;
  }
  if (!isIdentifier(Name)) {
    Error = ("callee `" + Name +
             "` is not a plain identifier, so a call through a member, a "
             "pointer or a macro-built name is outside the subset")
                .str();
    return std::nullopt;
  }

  llvm::SmallVector<llvm::StringRef, 4> Args;
  if (!splitArguments(S.substr(Open + 1, S.size() - Open - 2), Args)) {
    Error = "unbalanced brackets in the argument list";
    return std::nullopt;
  }

  std::vector<std::string> Bindings;
  std::string Src;
  llvm::raw_string_ostream OS(Src);
  OS << "callExpr(callee(functionDecl(hasName(\"" << Name << "\")))";
  // The count is pinned so that a pattern naming two arguments does not match a
  // call taking three. Coccinelle needs `...` inside the argument list to relax
  // this, which the subset does not have, so being strict is the honest choice.
  OS << ", argumentCountIs(" << Args.size() << ")";
  for (unsigned I = 0; I != Args.size(); ++I) {
    if (Args[I].empty()) {
      Error = "the argument list has an empty slot, so the pattern is not a "
              "call this compiler can constrain";
      return std::nullopt;
    }
    std::string ArgErr;
    const std::string M = argumentMatcher(Args[I], MetaVars, ArgErr);
    if (M.empty()) {
      Error = ArgErr;
      return std::nullopt;
    }
    OS << ", hasArgument(" << I << ", " << M << ")";
    if (const MetaVar *MV = findMetaVar(MetaVars, Args[I]))
      Bindings.push_back(MV->Name);
  }
  OS << ").bind(\"root\")";

  llvm::StringRef Consumable(Src);
  ast_matchers::dynamic::Diagnostics Diag;
  std::optional<ast_matchers::internal::DynTypedMatcher> M =
      ast_matchers::dynamic::Parser::parseMatcherExpression(Consumable, &Diag);
  if (!M) {
    Error = "the pattern compiled to matcher source that Clang rejected: " +
            Diag.toString() + " [source: " + Src + "]";
    return std::nullopt;
  }
  return CompiledPattern{*M, Src, Name.str(), std::move(Bindings)};
}

} // namespace clang::spatch

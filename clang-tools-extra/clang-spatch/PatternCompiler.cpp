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

const MetaVar *findMetaVar(llvm::ArrayRef<MetaVar> MetaVars,
                           llvm::StringRef Name) {
  for (const MetaVar &M : MetaVars)
    if (M.Name == Name)
      return &M;
  return nullptr;
}

/// The matcher source for one argument, or empty when the argument is outside
/// the subset.
///
/// \p Extra becomes the node matcher's leading clauses, which is how the
/// surrounded shape adds the exclusions that keep a callee and a C++ default
/// argument out of its enumeration.
std::string argumentMatcher(llvm::StringRef Arg,
                            llvm::ArrayRef<MetaVar> MetaVars,
                            llvm::StringRef Extra, std::string &Error) {
  const std::string Lead = Extra.empty() ? std::string() : (Extra + ", ").str();
  if (const MetaVar *M = findMetaVar(MetaVars, Arg)) {
    switch (M->Kind) {
    case MetaVar::Kind::Expression:
      return "expr(" + Extra.str() + ").bind(\"" + M->Name + "\")";
    case MetaVar::Kind::Constant:
      // Not `expr()`. A constant metavariable describes a literal or an
      // enumerator, and Coccinelle's own expected output for tests/constx.cocci
      // rewrites foo(12) and foo('a') while leaving foo(x) alone.
      return "expr(" + Lead +
             "anyOf(integerLiteral(), floatLiteral(), stringLiteral(), "
             "characterLiteral(), cxxBoolLiteral(), "
             "declRefExpr(to(enumConstantDecl())))).bind(\"" +
             M->Name + "\")";
    case MetaVar::Kind::Identifier:
      return "declRefExpr(" + Lead + "to(namedDecl())).bind(\"" + M->Name +
             "\")";
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
    return ("integerLiteral(" + Lead + "equals(" + llvm::Twine(Value) + "))")
        .str();

  Error = ("argument `" + Arg +
           "` is neither a declared metavariable nor an integer literal")
              .str();
  return {};
}

/// Splits \p Args on the commas that sit outside every bracket.
///
/// Returns false when the brackets do not balance, in which case \p Out holds
/// whatever was split before the imbalance.
bool splitArgumentList(llvm::StringRef Args,
                       llvm::SmallVectorImpl<llvm::StringRef> &Out) {
  int Depth = 0;
  size_t Start = 0;
  for (size_t I = 0, E = Args.size(); I != E; ++I) {
    const char C = Args[I];
    if (C == '(' || C == '[' || C == '{')
      ++Depth;
    else if (C == ')' || C == ']' || C == '}')
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

/// The id the surrounded shape binds the callee under, so that the enumeration
/// can exclude it by node identity. Chosen not to collide with a metavariable
/// name, which SmPL does not allow to contain a dot.
constexpr llvm::StringLiteral CalleeBindId = "spatch.callee";

} // namespace

ArgDotsShape argumentDotsShape(llvm::StringRef Args) {
  llvm::SmallVector<llvm::StringRef, 8> Parts;
  if (!splitArgumentList(Args, Parts))
    return ArgDotsShape::Other;
  llvm::SmallVector<unsigned, 4> Dots;
  for (unsigned I = 0, E = Parts.size(); I != E; ++I)
    if (Parts[I] == "...")
      Dots.push_back(I);
  if (Dots.empty())
    return ArgDotsShape::Other;
  if (Parts.size() == 1)
    return ArgDotsShape::Bare;
  // Every ellipsis at the tail leaves each named argument at a fixed index, so
  // positional constraints express the list exactly.
  if (Dots.front() != 0 && Dots.back() == Parts.size() - 1 &&
      Dots.size() == Parts.size() - Dots.front())
    return ArgDotsShape::Prefix;
  // One named argument between two ellipses is the only unanchored shape the
  // matcher language can enumerate. Two of them would have to be adjacent and
  // in order, and two enumerations give the cross product instead.
  if (Parts.size() == 3 && Dots.size() == 2 && Dots[0] == 0 && Dots[1] == 2)
    return ArgDotsShape::Surrounded;
  return ArgDotsShape::Other;
}

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
  if (findMetaVar(MetaVars, Name)) {
    // Matching the metavariable's own spelling would match a function that
    // happens to carry that name and miss every call the rule means, while
    // reporting that the pattern compiled.
    Error = ("callee `" + Name +
             "` is a metavariable, and matching a callee by a metavariable "
             "needs a binding over the callee rather than a name")
                .str();
    return std::nullopt;
  }

  llvm::SmallVector<llvm::StringRef, 4> Args;
  if (!splitArgumentList(S.substr(Open + 1, S.size() - Open - 2), Args)) {
    Error = "unbalanced brackets in the argument list";
    return std::nullopt;
  }

  const bool HasDots = llvm::is_contained(Args, "...");
  const ArgDotsShape Shape =
      HasDots ? argumentDotsShape(S.substr(Open + 1, S.size() - Open - 2))
              : ArgDotsShape::Other;
  if (HasDots && Shape == ArgDotsShape::Other) {
    Error = "the argument list puts a named argument after a `...`, which "
            "needs the argument's position and it is not determined";
    return std::nullopt;
  }

  std::vector<std::string> Bindings;
  std::string Src;
  llvm::raw_string_ostream OS(Src);
  OS << "callExpr(callee(functionDecl(hasName(\"" << Name << "\")))";

  if (!HasDots) {
    // The count is pinned so that a pattern naming two arguments does not
    // match a call taking three. A `...` in the list is what relaxes it.
    OS << ", argumentCountIs(" << Args.size() << ")";
  }

  if (Shape == ArgDotsShape::Surrounded) {
    // The one argument sits at no fixed index, so the matcher enumerates the
    // call's children and excludes the two that are not written arguments.
    // `forEach` yields one match per argument, which is what makes the
    // position existential rather than fixed at zero the way `hasAnyArgument`
    // would leave it.
    const llvm::StringRef Arg = Args[1];
    const std::string Exclusions = ("unless(equalsBoundNode(\"" + CalleeBindId +
                                    "\")), unless(cxxDefaultArgExpr())")
                                       .str();
    std::string ArgErr;
    const std::string M = argumentMatcher(Arg, MetaVars, Exclusions, ArgErr);
    if (M.empty()) {
      Error = ArgErr;
      return std::nullopt;
    }
    // The callee binding has to precede the enumeration, because
    // `equalsBoundNode` on an id that is not yet bound lets every node through.
    OS << ", callee(expr().bind(\"" << CalleeBindId << "\"))";
    OS << ", forEach(" << M << ")";
    if (const MetaVar *MV = findMetaVar(MetaVars, Arg))
      Bindings.push_back(MV->Name);
  } else {
    for (unsigned I = 0; I != Args.size(); ++I) {
      if (Args[I] == "...")
        break; // Bare and prefix shapes leave the tail unconstrained.
      if (Args[I].empty()) {
        Error = "the argument list has an empty slot, so the pattern is not a "
                "call this compiler can constrain";
        return std::nullopt;
      }
      std::string ArgErr;
      const std::string M = argumentMatcher(Args[I], MetaVars, "", ArgErr);
      if (M.empty()) {
        Error = ArgErr;
        return std::nullopt;
      }
      OS << ", hasArgument(" << I << ", " << M << ")";
      if (const MetaVar *MV = findMetaVar(MetaVars, Args[I]))
        Bindings.push_back(MV->Name);
    }
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

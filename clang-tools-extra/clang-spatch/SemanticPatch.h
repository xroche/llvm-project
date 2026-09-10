//===--- SemanticPatch.h - SmPL pattern representation ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The parsed form of a semantic patch, sitting between the SmPL parser and the
// pattern compiler so that neither depends on the other.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_SEMANTICPATCH_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_SEMANTICPATCH_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>
#include <vector>

namespace clang::spatch {

/// A metavariable declaration. Only the kinds this tool can compile appear
/// here; anything else is refused by the parser rather than represented.
struct MetaVar {
  enum class Kind {
    Expression,
    Identifier,
    Statement,
    /// A type metavariable. In the subset, but the compiler cannot bind it.
    Type,
    Constant,
    Position
  };
  Kind Kind;
  std::string Name;
  /// Set when the declaration inherits from an earlier rule, as in `r.p`.
  /// The name of that rule, with \c Name holding the metavariable alone.
  std::string InheritedFrom;
};

/// One line of a rule body.
struct PatternItem {
  enum class Kind {
    /// A C statement written as a pattern, held as source text.
    Statement,
    /// A statement-level `...`, with any `when` constraints attached.
    Dots,
    /// An alternation, `( a | b )` with each delimiter at column zero.
    Disjunction
  };
  /// How the line is marked in the patch. Context lines are unmarked.
  enum class Marker { Context, Minus, Plus, Star };

  /// One patch line's contribution to a grouped statement's \c Text.
  ///
  /// Grouping joins the lines of one side with a single space, so which part
  /// of \c Text a `-` line wrote cannot be recovered from the text. An edit
  /// inside the matched node needs exactly that: only the span the `-` lines
  /// wrote is replaced, and the rest of the statement stays as the target
  /// wrote it.
  struct Span {
    unsigned Offset = 0; ///< Into \c Text.
    unsigned Length = 0;
    /// The marker of the patch line this span came from.
    Marker LineMarker = Marker::Context;
    /// Line in the .cocci file, so that a `-` run and the `+` run beside it
    /// can be paired the way a line diff pairs them.
    unsigned Line = 0;
  };

  Kind Kind;
  Marker Marker = Marker::Context;
  /// The statement text, for Kind::Statement. Metavariable names appear in it
  /// verbatim and are resolved by the compiler.
  std::string Text;
  /// A position metavariable attached with `@p`, if any. Recorded by the
  /// parser. No consumer reads it yet, so a rule's reports are placed at its
  /// anchor rather than at the position the patch names.
  std::string PositionVar;
  /// The `when != X` constraints, for Kind::Dots. Each entry is the text of
  /// one forbidden construct.
  std::vector<std::string> WhenNot;
  /// `when any` suppresses the shortest-path restriction. Recorded by the
  /// parser. No consumer reads it yet.
  bool WhenAny = false;
  /// `when strict` requires the pattern to hold on paths that do not reach the
  /// end of the function. Recorded by the parser. No consumer reads it yet,
  /// which is why a path through a `noreturn` call is not distinguished.
  bool WhenStrict = false;
  /// For Kind::Disjunction, the alternative branches in source order.
  std::vector<std::vector<PatternItem>> Branches;
  /// True when grouping joined a `{` to the lines below it and to the `}`
  /// that closes them.
  ///
  /// Such an item is one construct written across lines, and the pattern
  /// parser needs to know so: `{ .a = E, }` is an initialiser list and no
  /// inner line of it parses on its own, so Clang has to be given a context
  /// that admits an initialiser element before it can say what the group is.
  bool BraceGroup = false;
  /// True when the statement is still incomplete after grouping, as in an
  /// `if` head whose body sits on the other side of the patch. Such an item
  /// is a fragment rather than a statement, and writing one back as a
  /// replacement would drop whatever completes it.
  bool Unfinished = false;
  /// Where each patch line's text sits inside \c Text, in source order. One
  /// entry for an ungrouped line, and one per absorbed line after grouping.
  std::vector<Span> Spans;
  /// Line in the .cocci file, for diagnostics.
  unsigned Line = 0;
};

/// A single rule.
///
/// The body is held three ways: as written, and grouped into statements once
/// per side. Two sides are needed because a context line belongs to both the
/// sequence the rule matches and the sequence that replaces it, so a
/// transformed multi-line statement interleaves `-`, `+` and unmarked lines
/// and no single sequence can hold it. `Minus` and `Plus` are derived from
/// `Body` by the parser and are what a consumer reads.
struct Rule {
  /// Empty for an unnamed rule.
  std::string Name;
  /// The rule-level quantifier. Absent means the patch did not state one, and
  /// this tool requires it whenever the body contains a `...`, because
  /// Coccinelle's default is a file-global property that does not port.
  enum class Quantifier { Exists, Forall };
  std::optional<Quantifier> Quant;
  /// The `depends on` expression as written. A single positive rule name is
  /// the only form the compiler acts on. An expression over virtuals may be
  /// more complex, as in `!context && patch`, and is kept verbatim rather than
  /// reduced, because dropping the rest would hide the condition under which
  /// the rule runs.
  std::string DependsOn;
  /// True when the dependency names a virtual rule rather than another rule,
  /// which is how a patch selects report or patch mode.
  bool DependsOnVirtual = false;
  std::vector<MetaVar> MetaVars;
  /// The rule body a line at a time, in source order and ungrouped.
  std::vector<PatternItem> Body;
  /// The context and `-` lines grouped into whole statements: the sequence
  /// the rule matches.
  std::vector<PatternItem> Minus;
  /// The context and `+` lines grouped into whole statements: the sequence
  /// that replaces it. A `*` rule states no replacement, so its plus side is
  /// its context lines and carries no meaning.
  std::vector<PatternItem> Plus;
  unsigned Line = 0;
};

/// A construct the parser recognised but cannot compile. Collected rather than
/// ignored, for the reason given at the top of tool/ClangSpatch.cpp.
struct Refusal {
  /// The SmPL construct, named as a user would recognise it, for example
  /// "script:ocaml rule" or "fresh identifier".
  std::string Construct;
  /// Why it cannot be compiled, in one sentence.
  std::string Reason;
  unsigned Line = 0;
};

/// A reporting rule, `@script:python@` or `@script:ocaml@`. It carries no
/// pattern and is never compiled to a matcher, so it is recorded apart from
/// the pattern rules: that keeps it out of the rule count the output reports
/// while still letting the output say the file had one.
struct ScriptRule {
  /// Empty for an unnamed rule.
  std::string Name;
  unsigned Line = 0;
};

/// Every pattern statement of one grouped side, branches included.
///
/// A branch's statements are pattern statements of the rule, so a reader that
/// stops at the top level sees none of them for a rule whose whole body is a
/// disjunction.
///
/// \p Groups, when given, takes \c PatternItem::BraceGroup for each statement
/// in the same order, which is what a caller passing the statements on to
/// \c parsePattern needs.
inline void collectStatements(const std::vector<PatternItem> &Side,
                              std::vector<std::string> &Out,
                              llvm::SmallVectorImpl<bool> *Groups = nullptr) {
  for (const PatternItem &I : Side) {
    if (I.Kind == PatternItem::Kind::Disjunction) {
      for (const std::vector<PatternItem> &Branch : I.Branches)
        collectStatements(Branch, Out, Groups);
      continue;
    }
    if (I.Kind != PatternItem::Kind::Statement)
      continue;
    Out.push_back(I.Text);
    if (Groups)
      Groups->push_back(I.BraceGroup);
  }
}

/// One `-` run of a grouped statement, paired with the `+` run beside it.
///
/// A rule that rewrites part of a statement interleaves its `-`, `+` and
/// context lines, so the statement has to be split the way a line diff splits
/// one: each `-` run and the `+` run next to it is one hunk, and the context
/// around them is text the target already holds.
struct PatternHunk {
  /// Where the `-` lines wrote, in the minus item's \c Text.
  unsigned MinusOffset = 0;
  unsigned MinusLength = 0;
  /// The `+` lines that replace them, as written in the plus item's \c Text.
  /// Empty when the hunk deletes.
  std::string PlusText;
};

/// The hunks of one grouped statement.
///
/// \p Minus and \p Plus must be the same statement as grouped for each side,
/// so their spans name the same patch lines. A `+` run that no `-` run
/// precedes is an insertion placed relative to the match rather than a
/// replacement of it, and it is left out of the result: \p Insertions counts
/// them, so a caller can refuse rather than drop the line without saying so.
/// A `*` line asks for no change and is read here as context.
inline std::vector<PatternHunk> pairHunks(const PatternItem &Minus,
                                          const PatternItem &Plus,
                                          unsigned &Insertions) {
  Insertions = 0;
  // One entry per marked patch line of the statement, in source order, so a
  // `-` run and the `+` run beside it are adjacent although each side was
  // grouped on its own.
  struct Row {
    const PatternItem::Span *Span;
    bool IsPlus;
  };
  std::vector<Row> Rows;
  for (const PatternItem::Span &Sp : Minus.Spans)
    if (Sp.LineMarker == PatternItem::Marker::Minus)
      Rows.push_back({&Sp, false});
  for (const PatternItem::Span &Sp : Plus.Spans)
    if (Sp.LineMarker == PatternItem::Marker::Plus)
      Rows.push_back({&Sp, true});
  llvm::stable_sort(Rows, [](const Row &A, const Row &B) {
    if (A.Span->Line != B.Span->Line)
      return A.Span->Line < B.Span->Line;
    return !A.IsPlus && B.IsPlus;
  });

  std::vector<PatternHunk> Hunks;
  for (size_t I = 0; I != Rows.size();) {
    if (Rows[I].IsPlus) {
      while (I != Rows.size() && Rows[I].IsPlus)
        ++I;
      ++Insertions;
      continue;
    }
    PatternHunk H;
    H.MinusOffset = Rows[I].Span->Offset;
    unsigned End = H.MinusOffset;
    // Grouping joins consecutive lines with one space, so a `-` span that
    // does not start there has a context span before it, and taking it into
    // the run would overwrite text no `-` line marked.
    for (; I != Rows.size() && !Rows[I].IsPlus &&
           (Rows[I].Span->Offset == H.MinusOffset ||
            Rows[I].Span->Offset == End + 1);
         ++I)
      End = Rows[I].Span->Offset + Rows[I].Span->Length;
    H.MinusLength = End - H.MinusOffset;
    if (I != Rows.size() && Rows[I].IsPlus) {
      const unsigned From = Rows[I].Span->Offset;
      unsigned To = From;
      for (; I != Rows.size() && Rows[I].IsPlus; ++I)
        To = Rows[I].Span->Offset + Rows[I].Span->Length;
      H.PlusText = Plus.Text.substr(From, To - From);
    }
    Hunks.push_back(std::move(H));
  }
  return Hunks;
}

struct SemanticPatch {
  /// Virtual rule names the patch declares, as in `virtual report`.
  std::vector<std::string> Virtuals;
  /// The names a `typedef X;` declaration says are type names.
  ///
  /// Such a name is not a metavariable: it stands for itself, and the
  /// declaration is there because Coccinelle's own C parser has no other way
  /// to tell a type name from a variable. The pattern parser has to declare
  /// it, or the pattern reaches Clang with an undeclared type.
  ///
  /// Held per patch rather than per rule, because Coccinelle's type table is,
  /// and `tests/wchar.cocci` relies on it: it declares its three names in the
  /// first rule's header and writes them in the second rule's body.
  std::vector<std::string> TypeNames;
  std::vector<Rule> Rules;
  /// Reporting rules, kept apart from pattern rules because they have no
  /// pattern and are never compiled.
  std::vector<ScriptRule> ScriptRules;
  std::vector<Refusal> Refusals;
  /// True when every construct in the file was understood.
  bool fullyUnderstood() const { return Refusals.empty(); }
};

} // namespace clang::spatch

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_SPATCH_SEMANTICPATCH_H

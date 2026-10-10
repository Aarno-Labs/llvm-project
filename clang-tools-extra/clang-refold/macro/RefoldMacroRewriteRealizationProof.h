//===--- RefoldMacroRewriteRealizationProof.h -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Refutation of a structure-preserving macro rewrite against B.
//
// A rewrite that keeps an invocation's callsite and respells some actuals
// re-expands to every cover token the formal dependence proof shows it keeps,
// in A order, with whatever the respelled formals produce in between.  When
// B's image of the cover cannot be read that way, the rewrite cannot realize
// the edits there, whichever alignment the token diff happened to choose.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROREWRITEREALIZATIONPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROREWRITEREALIZATIONPROOF_H

#include "edit/RefoldPatchTypes.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"
#include "source/RefoldDiffTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <utility>

namespace clang {
namespace refold {

class RefoldMacroTopology;

/// Proves that a structure-preserving rewrite of a macro invocation cannot
/// reproduce B over the invocation's cover.
class RefoldMacroRewriteRealizationProof {
public:
  /// Borrowed producer model, caller-graph index, token streams and hunks;
  /// all must outlive the proof object.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldMacroTopology &macroTopology;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    /// The pass's A/B token hunks, in ascending A order.
    llvm::ArrayRef<diffutils::Hunk> hunks;
  };

  explicit RefoldMacroRewriteRealizationProof(Dependencies deps)
      : deps_(deps) {}

  /// Return whether a rewrite of \p root that respells exactly the actuals of
  /// \p respelledFormals provably fails to reproduce B over \p root's cover.
  ///
  /// \p stagedHunks are the A ranges of the edits the rewrite must realize.
  /// Any other hunk of the cover belongs to an edit not yet planned, which a
  /// later patch for the same callsite may realize, so its tokens and
  /// position stay open.  The rewrite is refuted only when B's image of the
  /// cover cannot hold the kept tokens in A order with open material between
  /// them.  Kept tokens that macro rescanning could join to new material -- an
  /// identifier just before it, a parenthesized group just after it -- and the
  /// output of a descendant whose call may have been formed from substituted
  /// tokens are open as well.  New material with no A position is not
  /// modeled, so a rewrite is never refuted when `RespellsInvisibleOccurrence`
  /// or `ExpansionHasUnmodeledDefinition`.
  bool RewriteCannotRealize(const RefoldModel::MacroInvocation &root,
                            llvm::ArrayRef<uint32_t> respelledFormals,
                            llvm::ArrayRef<ATokenRange> stagedHunks) const;

private:
  /// Return whether a formal in \p respelledFormals has no standard
  /// occurrence recorded in \p root's cover: its actual expanded to no tokens,
  /// so its occurrences, which the rewrite may fill, have no A position.
  static bool
  RespellsInvisibleOccurrence(const RefoldModel::MacroInvocation &root,
                              llvm::ArrayRef<uint32_t> respelledFormals);

  /// Return whether \p root, or an invocation its expansion generated, has no
  /// recorded definition, as for a builtin, or a definition that uses
  /// `__VA_OPT__`, whose groups appear and vanish with the variadic actual.
  bool ExpansionHasUnmodeledDefinition(
      const RefoldModel::MacroInvocation &root) const;

  /// Return A boundaries \p aBegin and \p aEnd moved outward over each run of
  /// matched tokens spelled like the token at that edge, and over the hunks
  /// such a run reaches.  The diff may place an edit anywhere along a run of
  /// identical tokens, so material produced at an edge of the cover can be
  /// aligned past it.  Widening only weakens a refutation.
  std::pair<uint64_t, uint64_t> SlideOverIdenticalRuns(uint64_t aBegin,
                                                       uint64_t aEnd) const;

  /// Return the B-token half-open range bounded by the B images of A
  /// boundaries \p aBegin and \p aEnd, widened over any hunk that touches
  /// either boundary.
  std::pair<uint64_t, uint64_t> BImageOf(uint64_t aBegin, uint64_t aEnd) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROREWRITEREALIZATIONPROOF_H

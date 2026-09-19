//===--- RefoldSplitExpansionHunkRepair.h -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Owner-aware repair of token-hunk edges that split a macro expansion.  The
// engine runs it once on the published token diff, before structural tiling.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSPLITEXPANSIONHUNKREPAIR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSPLITEXPANSIONHUNKREPAIR_H

#include "source/RefoldDiffTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <vector>

namespace clang {
namespace refold {

class RefoldModel;
struct PPTok;

/// Move hunk edges off macro expansions the hunk only partially owns.
///
/// A realizer reconstructs source by re-emitting a callsite, so an expansion
/// must be wholly inside a hunk or wholly outside it.  When an edge lands
/// strictly inside one, no owner can realize the hunk: the source projection
/// widens to the complete invocation spelling while the replacement carries
/// only the fragment of expanded tokens inside the hunk.  Either the
/// difference is dropped silently -- `return NULL;` became `return );` when
/// an alignment boundary fell one token inside `NULL`'s `((void*)0)`
/// expansion -- or every owner refuses and the whole translation unit
/// escalates to raw B.
///
/// The token objective admits such a boundary because it scores lexemes, not
/// expansions: matching two tokens of `((void*)0)` against a `0` and a `)`
/// that an edit newly wrote is one match richer than leaving them unmatched,
/// and can be forced on every optimal path.
///
/// Two kinds of move resolve a split expansion, and this tries them in
/// preference order.  *Retraction* walks an edge inward across tokens that
/// are identical on both sides; it keeps the invocation preserved, so it is
/// preferred, and it is what restores a match the certifier left unforced
/// because a repeated spelling made it ambiguous.  It comes in two forms.
/// The edge inside the expansion retracts out of it, giving the expansion
/// back to the untouched region beside the hunk.  When that edge's tokens
/// differ, the opposite edge may instead retract up to the expansion's
/// boundary, giving the tokens outside it back and leaving the hunk contained
/// in the expansion, where the callsite realizers own it -- the shape an
/// edit rewriting the start of a macro argument produces, when a repeated
/// `(` leaves the certifier unable to say which side of the callsite the new
/// one belongs on.  *Widening* walks the edge outward to the expansion's own
/// boundary, taking the rest of the expansion into the hunk; it gives up that
/// one callsite's spelling and is the only move available when no retraction
/// is, which is what an edit that rewrites the expression around a callsite
/// produces.
///
/// Widening is sound because an absorbed token pair sits in the untouched run
/// between two hunks, matched to each other by the selected alignment: moving
/// such a pair across the edge leaves the edit script producing exactly the
/// same B.  It is admitted only on the A->B map's own evidence and never
/// moves an edge into the neighbouring hunk, for reasons the helper
/// documents.  When the walk arrives at a neighbour that holds part of the
/// same expansion, the two hunks are merged -- the neighbour's replacement,
/// the walked run, and this hunk's replacement still produce exactly the same
/// B -- and the walk continues from the merged edge, so an edit touching an
/// argument and the tokens past the callsite replaces the whole callsite.
/// An edge that cannot reach a whole-expansion boundary this way is left
/// alone for the ordinary realizer lattice, which refuses a partial cover --
/// so this repair never trades a refusal for a guess.
///
/// \p aToBMap is the selected alignment's A->B token map.
void repairHunkEdgesOutOfPartiallyOwnedMacroExpansions(
    const RefoldModel &model, llvm::ArrayRef<PPTok> aToks,
    llvm::ArrayRef<PPTok> bToks, llvm::ArrayRef<int64_t> aToBMap,
    std::vector<diffutils::Hunk> &hunks);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSPLITEXPANSIONHUNKREPAIR_H

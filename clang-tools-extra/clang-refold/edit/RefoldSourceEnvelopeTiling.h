//===--- RefoldSourceEnvelopeTiling.h ---------------*- C++ -*-===//
//
// Generic source-envelope tiling helpers for clang-refold edit
// construction.
//
// These utilities normalize caller-supplied source pieces into a
// deterministic non-overlapping envelope and drive gap tiling with a
// caller-specific neutral-range check.  Consumers are
// `include/RefoldIncludeMaterializer` and
// `edit/RefoldExpansionFallbackPlanner`.  This is caller-policy gap-
// discharge mechanics, not owner-state transition logic and not a
// theorem-proof service — hence the `edit/` location and the
// `SourceEnvelopeInterval` naming (a byte interval, not a discharge
// witness).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDSOURCEENVELOPETILING_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDSOURCEENVELOPETILING_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace clang {
namespace refold {

/// Half-open physical source-byte interval covered by a normalized
/// source-envelope piece.
struct SourceEnvelopeInterval {
  /// Inclusive physical source-byte offset.
  uint64_t begin = 0;
  /// Exclusive physical source-byte offset.
  uint64_t end = 0;
};

/// Physical source-byte interval accessors. Structural pieces expose begin/end;
/// merged TU source-byte ranges may be represented as plain pairs.
template <typename PieceT>
uint64_t sourceEnvelopePieceBegin(const PieceT &piece) {
  return piece.begin;
}

template <typename PieceT>
uint64_t sourceEnvelopePieceEnd(const PieceT &piece) {
  return piece.end;
}

inline uint64_t
sourceEnvelopePieceBegin(const std::pair<uint64_t, uint64_t> &piece) {
  return piece.first;
}

inline uint64_t
sourceEnvelopePieceEnd(const std::pair<uint64_t, uint64_t> &piece) {
  return piece.second;
}

/// Sort by physical source-byte interval, absorb caller-approved nested pieces,
/// and reject every other overlap as ambiguous.
template <typename PieceT, typename KindLess, typename NestedCovered>
bool normalizeSourceEnvelopePieces(llvm::SmallVectorImpl<PieceT> &pieces,
                                   KindLess kindLess,
                                   NestedCovered nestedCovered) {
  llvm::sort(pieces, [&](const PieceT &lhs, const PieceT &rhs) {
    const uint64_t lhsBegin = sourceEnvelopePieceBegin(lhs);
    const uint64_t rhsBegin = sourceEnvelopePieceBegin(rhs);
    if (lhsBegin != rhsBegin)
      return lhsBegin < rhsBegin;

    const uint64_t lhsEnd = sourceEnvelopePieceEnd(lhs);
    const uint64_t rhsEnd = sourceEnvelopePieceEnd(rhs);
    if (lhsEnd != rhsEnd)
      return lhsEnd > rhsEnd;

    if (kindLess(lhs, rhs))
      return true;
    if (kindLess(rhs, lhs))
      return false;
    return lhs.id < rhs.id;
  });

  llvm::SmallVector<PieceT, 8> outerPieces;
  for (const PieceT &piece : pieces) {
    const uint64_t pieceBegin = sourceEnvelopePieceBegin(piece);
    const uint64_t pieceEnd = sourceEnvelopePieceEnd(piece);
    if (outerPieces.empty() ||
        pieceBegin >= sourceEnvelopePieceEnd(outerPieces.back())) {
      outerPieces.push_back(piece);
      continue;
    }

    const PieceT &outer = outerPieces.back();
    if (sourceEnvelopePieceBegin(outer) <= pieceBegin &&
        pieceEnd <= sourceEnvelopePieceEnd(outer) &&
        nestedCovered(outer, piece)) {
      continue;
    }

    return false;
  }

  pieces.clear();
  pieces.append(outerPieces.begin(), outerPieces.end());
  return true;
}

/// Prove every physical source-byte gap between normalized source-envelope
/// pieces.
template <typename PieceT, typename ProveGap>
bool proveSourceEnvelopeGaps(const llvm::SmallVectorImpl<PieceT> &pieces,
                             ProveGap proveGap) {
  if (pieces.empty())
    return true;

  uint64_t cursor = sourceEnvelopePieceEnd(pieces.front());
  for (size_t idx = 1; idx < pieces.size(); ++idx) {
    const PieceT &piece = pieces[idx];
    const uint64_t pieceBegin = sourceEnvelopePieceBegin(piece);
    const uint64_t pieceEnd = sourceEnvelopePieceEnd(piece);
    if (pieceBegin < cursor)
      return false;
    if (pieceBegin > cursor && !proveGap(cursor, pieceBegin))
      return false;
    cursor = pieceEnd;
  }

  return true;
}

/// Normalize and tile one physical source-byte gap with caller-specific
/// neutral-gap and piece-consumption proofs.
template <typename PieceT, typename KindLess, typename NestedCovered,
          typename NeutralRange, typename ConsumePiece>
bool proveSourceEnvelopeGap(llvm::SmallVectorImpl<PieceT> &pieces,
                            uint64_t gapBegin, uint64_t gapEnd,
                            KindLess kindLess, NestedCovered nestedCovered,
                            NeutralRange neutralRange,
                            ConsumePiece consumePiece) {
  if (!normalizeSourceEnvelopePieces(pieces, kindLess, nestedCovered))
    return false;

  uint64_t cursor = gapBegin;
  for (const PieceT &piece : pieces) {
    const uint64_t pieceBegin = sourceEnvelopePieceBegin(piece);
    const uint64_t pieceEnd = sourceEnvelopePieceEnd(piece);
    if (pieceBegin < cursor)
      return false;
    if (!neutralRange(cursor, pieceBegin))
      return false;
    consumePiece(piece);
    cursor = pieceEnd;
  }
  return neutralRange(cursor, gapEnd);
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDSOURCEENVELOPETILING_H

//===--- RefoldSourceGapProof.cpp -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "source/RefoldSourceGapProof.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <limits>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

/// Set one optional diagnostic without changing proof behavior.
void setSourceGapFailure(std::string *reason, std::string message) {
  if (reason)
    *reason = std::move(message);
}

/// Normalize submitted pieces into one deterministic non-overlapping outer
/// cover.  A nested record disappears only when the containing piece explicitly
/// declares that it has already proved that nested class.
std::optional<std::vector<SourceGapProofPiece>> normalizeSourceGapPieces(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t gapBegin, uint64_t gapEnd,
    ArrayRef<SourceGapProofPiece> submitted,
    std::vector<size_t> &absorbedPayloadIndices, std::string *reason) {
  std::vector<SourceGapProofPiece> pieces(submitted.begin(), submitted.end());
  for (const SourceGapProofPiece &piece : pieces) {
    if (piece.end <= piece.begin || piece.begin < gapBegin ||
        piece.end > gapEnd || piece.nestingClass >= 64 ||
        !structureIndex.IsExactLexicalBoundary(piece.begin) ||
        !structureIndex.IsExactLexicalBoundary(piece.end)) {
      setSourceGapFailure(
          reason,
          formatv("source-gap piece payload={0} range=[{1},{2}) is not a "
                  "complete exact interval inside gap=[{3},{4})",
                  piece.payloadIndex, piece.begin, piece.end, gapBegin,
                  gapEnd).str());
      return std::nullopt;
    }
  }

  llvm::sort(pieces, [](const SourceGapProofPiece &lhs,
                        const SourceGapProofPiece &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    if (lhs.end != rhs.end)
      return lhs.end > rhs.end;
    if (lhs.kindOrder != rhs.kindOrder)
      return lhs.kindOrder < rhs.kindOrder;
    if (lhs.stableId != rhs.stableId)
      return lhs.stableId < rhs.stableId;
    return lhs.payloadIndex < rhs.payloadIndex;
  });

  std::vector<SourceGapProofPiece> outerPieces;
  outerPieces.reserve(pieces.size());
  for (const SourceGapProofPiece &piece : pieces) {
    if (outerPieces.empty() || piece.begin >= outerPieces.back().end) {
      outerPieces.push_back(piece);
      continue;
    }

    const SourceGapProofPiece &outer = outerPieces.back();
    if (outer.begin <= piece.begin && piece.end <= outer.end &&
        outer.Absorbs(piece)) {
      absorbedPayloadIndices.push_back(piece.payloadIndex);
      continue;
    }

    setSourceGapFailure(
        reason,
        formatv("source-gap pieces overlap without an explicit nesting "
                "theorem: outer payload={0} range=[{1},{2}) inner "
                "payload={3} range=[{4},{5})",
                outer.payloadIndex, outer.begin, outer.end,
                piece.payloadIndex, piece.begin, piece.end).str());
    return std::nullopt;
  }

  return outerPieces;
}

/// Shared implementation for indexed-trivia and caller-policy gap proofs.
std::optional<SourceGapProofResult> proveSourceGapImpl(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t gapBegin, uint64_t gapEnd,
    ArrayRef<SourceGapProofPiece> submittedPieces,
    function_ref<bool(uint64_t, uint64_t)> proveNeutralRange,
    function_ref<void(size_t)> consumeOuterPiece,
    uint64_t uncoveredProtectedStructureKinds, std::string *reason) {
  if (gapEnd < gapBegin || gapEnd > structureIndex.GetSourceSize() ||
      !structureIndex.IsProtectionCensusComplete() ||
      !structureIndex.IsExactLexicalBoundary(gapBegin) ||
      !structureIndex.IsExactLexicalBoundary(gapEnd)) {
    setSourceGapFailure(
        reason,
        formatv("source gap=[{0},{1}) has invalid bounds, lexical "
                "boundaries, or an incomplete preprocessing census",
                gapBegin, gapEnd).str());
    return std::nullopt;
  }

  SourceGapProofResult result;
  std::optional<std::vector<SourceGapProofPiece>> normalized =
      normalizeSourceGapPieces(structureIndex, gapBegin, gapEnd,
                               submittedPieces,
                               result.absorbedPiecePayloadIndices, reason);
  if (!normalized)
    return std::nullopt;

  result.protectedIntervals =
      structureIndex.FindOverlapping(gapBegin, gapEnd);

  for (const PreprocessingStructureInterval *interval :
       result.protectedIntervals) {
    if (!interval || interval->begin < gapBegin || interval->end > gapEnd) {
      setSourceGapFailure(
          reason,
          "protected preprocessing interval only partially intersects the "
          "source gap");
      return std::nullopt;
    }

    bool contained = false;
    bool partiallyCrossesPiece = false;
    for (const SourceGapProofPiece &piece : *normalized) {
      if (piece.end <= interval->begin)
        continue;
      if (interval->end <= piece.begin)
        break;
      if (piece.begin <= interval->begin && interval->end <= piece.end) {
        contained = true;
        break;
      }
      partiallyCrossesPiece = true;
      break;
    }

    if (partiallyCrossesPiece) {
      setSourceGapFailure(
          reason,
          formatv("protected preprocessing interval kind={0} range=[{1},{2}) "
                  "partially crosses one caller piece",
                  toString(interval->kind), interval->begin, interval->end)
              .str());
      return std::nullopt;
    }
    if (contained)
      continue;

    const uint64_t kindBit =
        sourceGapProtectedStructureKindBit(interval->kind);
    if (kindBit == 0 ||
        (uncoveredProtectedStructureKinds & kindBit) == 0) {
      setSourceGapFailure(
          reason,
          formatv("protected preprocessing interval kind={0} range=[{1},{2}) "
                  "has no caller-authorized source-gap owner",
                  toString(interval->kind), interval->begin, interval->end)
              .str());
      return std::nullopt;
    }
  }

  uint64_t cursor = gapBegin;
  for (const SourceGapProofPiece &piece : *normalized) {
    if (piece.begin < cursor || !proveNeutralRange(cursor, piece.begin)) {
      setSourceGapFailure(
          reason,
          formatv("source-gap neutral range=[{0},{1}) was not proved",
                  cursor, piece.begin).str());
      return std::nullopt;
    }
    consumeOuterPiece(piece.payloadIndex);
    result.outerPiecePayloadIndices.push_back(piece.payloadIndex);
    cursor = piece.end;
  }

  if (!proveNeutralRange(cursor, gapEnd)) {
    setSourceGapFailure(
        reason,
        formatv("source-gap trailing neutral range=[{0},{1}) was not proved",
                cursor, gapEnd).str());
    return std::nullopt;
  }

  return result;
}

} // namespace

std::optional<std::pair<uint64_t, uint64_t>>
findSourceGapProducerInterval(
    const RefoldPreprocessingStructureIndex &structureIndex,
    PreprocessingStructureModelKind modelKind, uint64_t modelItemId) {
  const PreprocessingStructureInterval *matched = nullptr;
  for (const PreprocessingStructureInterval &interval :
       structureIndex.GetIntervals()) {
    if (interval.modelKind != modelKind || !interval.modelItemId ||
        *interval.modelItemId != modelItemId) {
      continue;
    }
    if (matched)
      return std::nullopt;
    matched = &interval;
  }
  if (!matched || !matched->IsValid())
    return std::nullopt;
  return std::make_pair(matched->begin, matched->end);
}

std::optional<std::pair<uint64_t, uint64_t>>
findSourceGapConditionalGroupRange(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t conditionalGroupId) {
  const PreprocessingStructureInterval *opening = nullptr;
  const PreprocessingStructureInterval *ending = nullptr;

  for (const PreprocessingStructureInterval &interval :
       structureIndex.GetIntervals()) {
    if (interval.modelKind !=
            PreprocessingStructureModelKind::ConditionalDirective ||
        interval.conditionalGroupId != conditionalGroupId) {
      continue;
    }

    const bool isOpening =
        interval.kind == PreprocessingStructureKind::ConditionalIf ||
        interval.kind == PreprocessingStructureKind::ConditionalIfdef ||
        interval.kind == PreprocessingStructureKind::ConditionalIfndef;
    if (isOpening) {
      if (opening)
        return std::nullopt;
      opening = &interval;
      continue;
    }
    if (interval.kind == PreprocessingStructureKind::ConditionalEndif) {
      if (ending)
        return std::nullopt;
      ending = &interval;
    }
  }

  if (!opening || !ending || !opening->IsValid() || !ending->IsValid() ||
      ending->end <= opening->begin) {
    return std::nullopt;
  }
  return std::make_pair(opening->begin, ending->end);
}

uint64_t sourceGapProtectedStructureKindBit(
    PreprocessingStructureKind kind) {
  const unsigned ordinal = static_cast<unsigned>(kind);
  if (ordinal >= std::numeric_limits<uint64_t>::digits)
    return 0;
  return uint64_t{1} << ordinal;
}

uint64_t sourceGapConditionalDirectiveKindMask() {
  return sourceGapProtectedStructureKindBit(
             PreprocessingStructureKind::ConditionalIf) |
         sourceGapProtectedStructureKindBit(
             PreprocessingStructureKind::ConditionalIfdef) |
         sourceGapProtectedStructureKindBit(
             PreprocessingStructureKind::ConditionalIfndef) |
         sourceGapProtectedStructureKindBit(
             PreprocessingStructureKind::ConditionalElif) |
         sourceGapProtectedStructureKindBit(
             PreprocessingStructureKind::ConditionalElifdef) |
         sourceGapProtectedStructureKindBit(
             PreprocessingStructureKind::ConditionalElifndef) |
         sourceGapProtectedStructureKindBit(
             PreprocessingStructureKind::ConditionalElse) |
         sourceGapProtectedStructureKindBit(
             PreprocessingStructureKind::ConditionalEndif);
}

std::optional<SourceGapProofResult>
proveSourceGapWithIndexedStructureAndTrivia(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t gapBegin, uint64_t gapEnd, std::string *reason) {
  std::vector<const PreprocessingStructureInterval *> intervals =
      structureIndex.FindOverlapping(gapBegin, gapEnd);

  // Physical preservation of an outer protected interval necessarily
  // preserves every wholly nested protected record.  Use one nesting class so
  // the common normalizer may collapse exact containment while still rejecting
  // every partial overlap.  The payload index is stable source-index order and
  // is returned only for diagnostics; callers use `protectedIntervals` as the
  // complete semantic inventory.
  SmallVector<SourceGapProofPiece, 8> pieces;
  pieces.reserve(intervals.size());
  for (size_t intervalIndex = 0; intervalIndex < intervals.size();
       ++intervalIndex) {
    const PreprocessingStructureInterval *interval = intervals[intervalIndex];
    if (!interval) {
      setSourceGapFailure(reason,
                          "source-gap structure inventory contains null entry");
      return std::nullopt;
    }
    pieces.push_back(SourceGapProofPiece{
        interval->begin, interval->end,
        interval->modelItemId.value_or(intervalIndex),
        static_cast<uint32_t>(interval->kind),
        /*nestingClass=*/0, /*absorbedNestingClasses=*/uint64_t{1},
        intervalIndex});
  }

  return proveSourceGapWithIndexedTrivia(structureIndex, gapBegin, gapEnd,
                                         pieces, reason);
}

std::optional<SourceGapProofResult> proveSourceGapWithIndexedTrivia(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t gapBegin, uint64_t gapEnd,
    ArrayRef<SourceGapProofPiece> pieces, std::string *reason) {
  return proveSourceGapImpl(
      structureIndex, gapBegin, gapEnd, pieces,
      [&](uint64_t begin, uint64_t end) {
        return structureIndex.IsRangeLexicallyIgnorable(begin, end);
      },
      [](size_t) {}, /*uncoveredProtectedStructureKinds=*/0, reason);
}

std::optional<SourceGapProofResult> proveSourceGapWithPolicy(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t gapBegin, uint64_t gapEnd,
    ArrayRef<SourceGapProofPiece> pieces,
    function_ref<bool(uint64_t, uint64_t)> proveNeutralRange,
    function_ref<void(size_t)> consumeOuterPiece,
    uint64_t uncoveredProtectedStructureKinds, std::string *reason) {
  return proveSourceGapImpl(
      structureIndex, gapBegin, gapEnd, pieces, proveNeutralRange,
      consumeOuterPiece, uncoveredProtectedStructureKinds, reason);
}

} // namespace refold
} // namespace clang

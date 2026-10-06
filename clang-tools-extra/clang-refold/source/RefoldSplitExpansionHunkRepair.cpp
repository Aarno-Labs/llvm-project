//===--- RefoldSplitExpansionHunkRepair.cpp ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Owner-aware repair of token-hunk edges that split a macro expansion or an
// include instance.  The engine runs it once on the published token diff,
// before structural tiling.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldSplitExpansionHunkRepair.h"

#include "model/RefoldModel.h"
#include "model/RefoldToken.h"
#include "support/RefoldLog.h"

#include <cstddef>
#include <optional>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Return the macro expansion an A-token position falls strictly inside, or
/// null when the position is an expansion boundary or outside every expansion.
///
/// Boundaries are deliberately not interior: a hunk edge that coincides with a
/// span edge already owns whole expansions on both sides.
/// A hunk edge strictly inside an expansion is not by itself a problem: an edit
/// confined to a macro argument has both edges inside the expansion and is
/// realized by replaying the callsite, which owns the whole expansion.  The
/// unrealizable shape is a *straddle*, where the hunk holds part of an
/// expansion and the neighbouring untouched region holds the rest, so neither
/// can reproduce it.  \p oppositeEdgeInsideSpan distinguishes the two: it is
/// true when the hunk's other edge also lies within the span, meaning the hunk
/// is contained rather than straddling.
const RefoldModel::PPSpan *
macroExpansionStraddledAtEdge(const RefoldModel &model, uint64_t edge,
                              uint64_t oppositeEdge, bool edgeIsLeft) {
  for (const RefoldModel::MacroInvocation &invocation :
       model.GetMacroInvocations()) {
    if (!invocation.invB || !invocation.invE)
      continue;
    for (const RefoldModel::PPSpan &span : invocation.spans) {
      if (!(span.begin < edge && edge < span.end))
        continue;
      // Contained: the whole hunk lies within this expansion, so the callsite
      // realizers own it and the edge is legitimate.
      const bool containedInSpan = edgeIsLeft ? (oppositeEdge <= span.end)
                                              : (span.begin <= oppositeEdge);
      if (containedInSpan)
        continue;
      return &span;
    }
  }
  return nullptr;
}

/// Return whether two A/B tokens are the same lexeme, so retracting a hunk edge
/// across them restores a match rather than inventing one.
bool aAndBTokensAreIdentical(ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
                             uint64_t aToken, uint64_t bToken) {
  if (aToken >= aToks.size() || bToken >= bToks.size())
    return false;
  return aToks[static_cast<size_t>(aToken)].kind ==
             bToks[static_cast<size_t>(bToken)].kind &&
         aToks[static_cast<size_t>(aToken)].spelling ==
             bToks[static_cast<size_t>(bToken)].spelling;
}

/// Return whether the selected alignment matched this exact A/B token pair, so
/// widening a hunk edge across it moves a pair the alignment already chose.
bool alignmentMatchedAToB(ArrayRef<int64_t> aToBMap, uint64_t aToken,
                          uint64_t bToken) {
  return aToken < aToBMap.size() &&
         aToBMap[static_cast<size_t>(aToken)] == static_cast<int64_t>(bToken);
}

/// Result of walking one hunk edge outward to the boundary of the macro
/// expansion it splits.
struct SplitExpansionWidening {
  /// Tokens the edge moves outward, identically on the A and B sides.
  uint64_t distance = 0;
  /// The walk arrived at the neighbouring hunk's edge on both sides while that
  /// edge still splits the expansion, so the expansion is shared with the
  /// neighbour and cannot be consumed without it.  The caller merges the two
  /// hunks and walks again from the merged edge.
  bool reachesNeighbour = false;
};

/// Return how far one hunk edge must move *outward* for the macro expansion it
/// splits to sit wholly inside the hunk, or `std::nullopt` when no reachable
/// position achieves that.
///
/// This is the second of the two moves that resolve a split expansion, and the
/// only one available when the tokens at the edge differ between A and B.
/// Retraction hands the expansion back to the untouched region beside the hunk;
/// widening takes the rest of it into the hunk, which is what an edit that
/// genuinely consumes the whole callsite means.  A token absorbed this way sits
/// in the untouched run between two hunks, where the alignment matched it
/// against exactly the B token now on the other side of the edge: moving that
/// pair across the edge leaves the edit script producing exactly the same B.
///
/// Two independent facts establish the pairing, and both are required.  The
/// selected A->B map must name the pair, which is what says the two tokens
/// correspond at all -- the hunk list alone does not, because an insert-only
/// hunk whose matched edge context was trimmed leaves matched B tokens outside
/// every hunk with no A token beside them.  The neighbouring hunk's edge must
/// also not be crossed, because an earlier repair may have taken an anchored
/// pair into that hunk, and the map still records the pair the alignment chose
/// rather than the ownership the repair established.
///
/// So the walk never moves an edge *into* the neighbouring hunk.  When it
/// arrives exactly at the neighbour's edge on both sides and the expansion
/// still extends past it, the neighbour holds part of the same expansion, and
/// the result reports `reachesNeighbour`.  Merging the two hunks is then sound
/// without consulting the map inside the neighbour: the merged replacement is
/// the neighbour's own replacement, the walked run of matched identical pairs,
/// and this hunk's replacement, in order, so it produces exactly the same B.
///
/// \p aFloor and \p bFloor are the preceding hunk's exclusive A/B ends, or zero
/// for the first hunk; \p floorIsHunkEdge says which.
static std::optional<SplitExpansionWidening>
leftEdgeWidenDistanceOutOfSplitExpansion(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    ArrayRef<int64_t> aToBMap, const diffutils::Hunk &hunk, uint64_t aFloor,
    uint64_t bFloor, bool floorIsHunkEdge) {
  uint64_t aStart = hunk.aStart;
  uint64_t bStart = hunk.bStart;
  while (macroExpansionStraddledAtEdge(model, aStart, hunk.aEnd,
                                       /*edgeIsLeft=*/true)) {
    if (floorIsHunkEdge && aStart == aFloor && bStart == bFloor)
      return SplitExpansionWidening{hunk.aStart - aStart,
                                    /*reachesNeighbour=*/true};
    if (aStart <= aFloor || bStart <= bFloor)
      return std::nullopt;
    --aStart;
    --bStart;
    if (!aAndBTokensAreIdentical(aToks, bToks, aStart, bStart) ||
        !alignmentMatchedAToB(aToBMap, aStart, bStart))
      return std::nullopt;
  }
  return SplitExpansionWidening{hunk.aStart - aStart,
                                /*reachesNeighbour=*/false};
}

/// Right-edge counterpart of `leftEdgeWidenDistanceOutOfSplitExpansion`.
///
/// \p aLimit and \p bLimit are the following hunk's A/B starts, or the A/B
/// token counts for the last hunk; \p limitIsHunkEdge says which.
static std::optional<SplitExpansionWidening>
rightEdgeWidenDistanceOutOfSplitExpansion(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    ArrayRef<int64_t> aToBMap, const diffutils::Hunk &hunk, uint64_t aLimit,
    uint64_t bLimit, bool limitIsHunkEdge) {
  uint64_t aEnd = hunk.aEnd;
  uint64_t bEnd = hunk.bEnd;
  while (macroExpansionStraddledAtEdge(model, aEnd, hunk.aStart,
                                       /*edgeIsLeft=*/false)) {
    if (limitIsHunkEdge && aEnd == aLimit && bEnd == bLimit)
      return SplitExpansionWidening{aEnd - hunk.aEnd,
                                    /*reachesNeighbour=*/true};
    if (aEnd >= aLimit || bEnd >= bLimit)
      return std::nullopt;
    if (!aAndBTokensAreIdentical(aToks, bToks, aEnd, bEnd) ||
        !alignmentMatchedAToB(aToBMap, aEnd, bEnd))
      return std::nullopt;
    ++aEnd;
    ++bEnd;
  }
  return SplitExpansionWidening{aEnd - hunk.aEnd, /*reachesNeighbour=*/false};
}

/// Return how far a hunk's left edge must move *inward* for the hunk to lie
/// wholly inside the macro expansion its right edge splits, zero when the right
/// edge splits no expansion, or `std::nullopt` when no move achieves that.
///
/// This is retraction applied to the edge that does not sit inside the
/// expansion.  A hunk that begins in tokens outside an expansion and ends
/// inside it is a straddle: the expansion's owner cannot realize the tokens
/// before it, and the owner of those tokens cannot realize a partial expansion.
/// Ordinary retraction hands the expansion's part back to the untouched region
/// on the right, and is unavailable when the tokens at that edge differ between
/// A and B -- which is what an edit rewriting the start of a macro argument
/// produces.  The tokens *before* the expansion may still be identical on both
/// sides, and handing those back to the untouched region on the left leaves the
/// hunk inside the expansion, where the callsite realizers own it and the
/// invocation is kept.
///
/// Every token handed back must be the same lexeme on both sides, exactly as
/// for ordinary retraction, so the edit script still produces the same B.  The
/// B side must stay non-empty: emptying it turns a replacement into a deletion,
/// which belongs to the owner-aligned deletion slide.  The move is taken only
/// when neither edge of the result splits an expansion; when the right edge
/// sits inside nested expansions the left edge walks to the innermost boundary.
static std::optional<uint64_t> leftEdgeContainDistanceIntoSplitExpansion(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    const diffutils::Hunk &hunk) {
  const RefoldModel::PPSpan *span = macroExpansionStraddledAtEdge(
      model, hunk.aEnd, hunk.aStart, /*edgeIsLeft=*/false);
  if (!span)
    return 0;

  uint64_t aStart = hunk.aStart;
  uint64_t bStart = hunk.bStart;
  while (span) {
    // A straddle at the right edge means the span begins after the left edge
    // and before the right one, so the walk stays inside the hunk.
    const uint64_t distance = span->begin - aStart;
    if (distance >= hunk.bEnd - bStart)
      return std::nullopt;
    for (uint64_t offset = 0; offset < distance; ++offset)
      if (!aAndBTokensAreIdentical(aToks, bToks, aStart + offset,
                                   bStart + offset))
        return std::nullopt;
    aStart += distance;
    bStart += distance;
    span = macroExpansionStraddledAtEdge(model, hunk.aEnd, aStart,
                                         /*edgeIsLeft=*/false);
  }
  if (macroExpansionStraddledAtEdge(model, aStart, hunk.aEnd,
                                    /*edgeIsLeft=*/true))
    return std::nullopt;
  return aStart - hunk.aStart;
}

/// Right-edge counterpart of `leftEdgeContainDistanceIntoSplitExpansion`: how
/// far the right edge must move inward for the hunk to lie wholly inside the
/// expansion its left edge splits.
static std::optional<uint64_t> rightEdgeContainDistanceIntoSplitExpansion(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    const diffutils::Hunk &hunk) {
  const RefoldModel::PPSpan *span = macroExpansionStraddledAtEdge(
      model, hunk.aStart, hunk.aEnd, /*edgeIsLeft=*/true);
  if (!span)
    return 0;

  uint64_t aEnd = hunk.aEnd;
  uint64_t bEnd = hunk.bEnd;
  while (span) {
    // A straddle at the left edge means the span ends after the left edge and
    // before the right one, so the walk stays inside the hunk.
    const uint64_t distance = aEnd - span->end;
    if (distance >= bEnd - hunk.bStart)
      return std::nullopt;
    for (uint64_t offset = 1; offset <= distance; ++offset)
      if (!aAndBTokensAreIdentical(aToks, bToks, aEnd - offset, bEnd - offset))
        return std::nullopt;
    aEnd -= distance;
    bEnd -= distance;
    span = macroExpansionStraddledAtEdge(model, hunk.aStart, aEnd,
                                         /*edgeIsLeft=*/true);
  }
  if (macroExpansionStraddledAtEdge(model, aEnd, hunk.aStart,
                                    /*edgeIsLeft=*/false))
    return std::nullopt;
  return hunk.aEnd - aEnd;
}

/// Return the include instance whose A-token cover a hunk's left edge falls
/// strictly inside while the hunk ends past that cover, or null.
///
/// This is `macroExpansionStraddledAtEdge` for include instances, left edge
/// only.  A cover boundary is not interior, so an edge on one already owns
/// whole instances on both sides; and a hunk that also ends within the cover is
/// contained in the instance, where the include's own realizers own it.
/// Covers are half-open, so a token-less instance straddles nothing.
const RefoldModel::IncludeItem *
includeStraddledAtLeftEdge(const RefoldModel &model, uint64_t aStart,
                           uint64_t aEnd) {
  for (const RefoldModel::IncludeItem &include : model.GetIncludes()) {
    const RefoldModel::PPCover &cover = include.cover;
    if (cover.begin < aStart && aStart < cover.end && cover.end < aEnd)
      return &include;
  }
  return nullptr;
}

/// Return whether one owner covers every A token of the deletion
/// `[aBegin, aEnd)`.
///
/// A retraction that consumes a hunk's whole B side no longer decides only
/// where an edge falls: B keeps none of the hunk's tokens, so the retraction
/// picks which of several identically spelled A tokens are deleted.  That pick
/// is an improvement only when the deletion it leaves is one owner's, which is
/// the property the owner-aligned deletion slide requires of a deletion it
/// moves.  A deletion that still holds an include instance beside the
/// including file's tokens straddles the boundary the retraction exists to
/// clear, and may cross the directives between them.
static bool deletionHasOneOwner(const RefoldModel &model, uint64_t aBegin,
                                uint64_t aEnd) {
  if (aEnd <= aBegin)
    return false;
  const std::optional<uint64_t> owner = model.InnermostIncludeAtPP(aBegin);
  for (uint64_t aToken = aBegin + 1; aToken < aEnd; ++aToken)
    if (model.InnermostIncludeAtPP(aToken) != owner)
      return false;
  return true;
}

/// Where a hunk whose left edge straddles an include instance keeps the
/// instance's trailing tokens.
struct IncludeTailKeep {
  /// The first A token past the outermost straddled cover.  The hunk's A tokens
  /// `[aStart, tailEnd)` are the tail, and they are kept untouched.
  uint64_t tailEnd = 0;
  /// The first B token of the run that reproduces the tail.  It is the hunk's
  /// `bStart` when the edge simply retracts; past it, the B tokens before it
  /// are inserted at the edge, inside the include.
  uint64_t tailCopyBegin = 0;
};

/// Return where a hunk whose left edge straddles an include instance keeps the
/// instance's trailing tokens, or `std::nullopt` when it straddles none or the
/// tail cannot be kept.
///
/// The tail is kept only where B reproduces it: as a contiguous run of the
/// same lexemes inside the hunk's B side, so the untouched region reproduces
/// that run through the kept tokens.  When the run begins the B side the edge
/// simply retracts across it.  When B first writes tokens of its own -- an edit
/// adding a parameter to a header declaration does, before the closing `);` --
/// the hunk splits around the run: the B tokens before it are inserted at the
/// edge, where both neighbouring A tokens belong to the include, and the B
/// tokens after it replace the including file's tokens.  The three pieces
/// concatenate to exactly the hunk's B side, so the edit script still produces
/// the same B, and no piece holds tokens of both the include and the including
/// file.
///
/// The first run is taken.  Every run keeps the same tail, and the first one
/// inserts the fewest B tokens into the include and leaves the most to the
/// including file's replacement, which is the order the owner-depth tie-break
/// ranks alignments in; a later run would only move more of B into the deeper
/// owner.
///
/// The run may end the B side only when the deletion it leaves has one owner;
/// see `deletionHasOneOwner`.  No later run exists then, so that refusal is
/// final.
static std::optional<IncludeTailKeep>
leftEdgeTailKeepOutOfSplitInclude(const RefoldModel &model,
                                  ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
                                  const diffutils::Hunk &hunk) {
  const std::optional<uint64_t> tailEnd =
      straddledIncludeTailEnd(model, hunk.aStart, hunk.aEnd);
  if (!tailEnd)
    return std::nullopt;
  const std::optional<uint64_t> copyBegin = firstBRunReproducingA(
      aToks, bToks, hunk.aStart, *tailEnd, hunk.bStart, hunk.bEnd);
  if (!copyBegin)
    return std::nullopt;
  if (*copyBegin + (*tailEnd - hunk.aStart) == hunk.bEnd &&
      !deletionHasOneOwner(model, *tailEnd, hunk.aEnd))
    return std::nullopt;
  return IncludeTailKeep{*tailEnd, *copyBegin};
}

/// Return the first A token of the outermost include instance whose cover ends
/// exactly at a hunk's right edge and begins after its left edge, or nullopt.
///
/// Such a hunk holds that instance whole as its trailing tokens, together with
/// part of the including file before it.  The owner-depth tie-break leaves the
/// instance unforced when deleting it is free, which is the case for a header
/// whose only token sits in a gap the including file owns -- an included value
/// in an initializer list, for one.  Nested instances end no later than the
/// instance containing them, so the outermost one is the one with the earliest
/// begin.
std::optional<uint64_t> wholeIncludeBeginAtRightEdge(const RefoldModel &model,
                                                     uint64_t aStart,
                                                     uint64_t aEnd) {
  std::optional<uint64_t> begin;
  for (const RefoldModel::IncludeItem &include : model.GetIncludes()) {
    const RefoldModel::PPCover &cover = include.cover;
    if (cover.begin < cover.end && aStart < cover.begin && cover.end == aEnd &&
        (!begin || cover.begin < *begin))
      begin = cover.begin;
  }
  return begin;
}

/// Return how far a hunk's right edge must move inward to hand back every
/// include instance it holds whole at that edge, zero when it holds none, or
/// `std::nullopt` when the move is not admissible.
///
/// The admissibility conditions are those of a left-edge retraction by
/// `leftEdgeTailKeepOutOfSplitInclude`: every crossed token pair is the same
/// lexeme on both sides, so the untouched region after the hunk reproduces it
/// through the kept `#include`, and the walk consumes the whole B side only
/// when the deletion it leaves has one owner.  Adjacent instances at the edge
/// are handed back one after another.
static std::optional<uint64_t> rightEdgeRetractDistanceOutOfWholeInclude(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    const diffutils::Hunk &hunk) {
  uint64_t aEnd = hunk.aEnd;
  uint64_t bEnd = hunk.bEnd;
  while (std::optional<uint64_t> coverBegin =
             wholeIncludeBeginAtRightEdge(model, hunk.aStart, aEnd)) {
    const uint64_t distance = aEnd - *coverBegin;
    if (distance > bEnd - hunk.bStart)
      return std::nullopt;
    for (uint64_t offset = 1; offset <= distance; ++offset)
      if (!aAndBTokensAreIdentical(aToks, bToks, aEnd - offset, bEnd - offset))
        return std::nullopt;
    aEnd -= distance;
    bEnd -= distance;
  }
  if (bEnd == hunk.bStart && !deletionHasOneOwner(model, hunk.aStart, aEnd))
    return std::nullopt;
  return hunk.aEnd - aEnd;
}

} // namespace

std::optional<uint64_t> straddledIncludeTailEnd(const RefoldModel &model,
                                                uint64_t aStart,
                                                uint64_t aEnd) {
  std::optional<uint64_t> tailEnd;
  while (const RefoldModel::IncludeItem *include =
             includeStraddledAtLeftEdge(model, aStart, aEnd)) {
    aStart = include->cover.end;
    tailEnd = aStart;
  }
  return tailEnd;
}

std::optional<uint64_t> firstBRunReproducingA(ArrayRef<PPTok> aToks,
                                              ArrayRef<PPTok> bToks,
                                              uint64_t aBegin, uint64_t aEnd,
                                              uint64_t bBegin, uint64_t bEnd) {
  const uint64_t length = aEnd - aBegin;
  for (uint64_t copyBegin = bBegin; copyBegin + length <= bEnd; ++copyBegin) {
    bool reproduced = true;
    for (uint64_t offset = 0; reproduced && offset < length; ++offset)
      reproduced = aAndBTokensAreIdentical(aToks, bToks, aBegin + offset,
                                           copyBegin + offset);
    if (reproduced)
      return copyBegin;
  }
  return std::nullopt;
}

void repairHunkEdgesOutOfPartiallyOwnedIncludes(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    std::vector<diffutils::Hunk> &hunks) {
  for (size_t index = 0; index < hunks.size(); ++index) {
    // Only a replacement is repaired here, for the reasons given in
    // `repairHunkEdgesOutOfPartiallyOwnedMacroExpansions`.
    if (hunks[index].aStart >= hunks[index].aEnd ||
        hunks[index].bStart >= hunks[index].bEnd)
      continue;

    if (std::optional<IncludeTailKeep> keep = leftEdgeTailKeepOutOfSplitInclude(
            model, aToks, bToks, hunks[index])) {
      const diffutils::Hunk original = hunks[index];
      const uint64_t tailLength = keep->tailEnd - original.aStart;
      if (keep->tailCopyBegin == original.bStart) {
        REFOLD_LOG_DEBUG(
            "plan/hunk-edge",
            "hunk #{0} A=[{1},{2}) starts inside an include instance it does "
            "not end in; retracting the left edge by {3} identical token(s) to "
            "A={4} so the include is left untouched",
            index, original.aStart, original.aEnd, tailLength, keep->tailEnd);
      } else {
        REFOLD_LOG_DEBUG(
            "plan/hunk-edge",
            "hunk #{0} A=[{1},{2}) starts inside an include instance it does "
            "not end in; B reproduces the instance's {3} trailing token(s) at "
            "B={4}, so inserting B=[{5},{4}) at A={1} inside the include and "
            "resuming the hunk at A={6}",
            index, original.aStart, original.aEnd, tailLength,
            keep->tailCopyBegin, original.bStart, keep->tailEnd);
        hunks.insert(hunks.begin() + index,
                     diffutils::Hunk{original.aStart, original.aStart,
                                     original.bStart, keep->tailCopyBegin});
        ++index;
      }
      hunks[index].aStart = keep->tailEnd;
      hunks[index].bStart = keep->tailCopyBegin + tailLength;
    }

    diffutils::Hunk &hunk = hunks[index];
    if (std::optional<uint64_t> distance =
            rightEdgeRetractDistanceOutOfWholeInclude(model, aToks, bToks,
                                                      hunk);
        distance && *distance != 0) {
      REFOLD_LOG_DEBUG(
          "plan/hunk-edge",
          "hunk #{0} A=[{1},{2}) ends with a whole include instance it begins "
          "before; retracting the right edge by {3} identical token(s) to "
          "A={4} so the include is left untouched",
          index, hunk.aStart, hunk.aEnd, *distance, hunk.aEnd - *distance);
      hunk.aEnd -= *distance;
      hunk.bEnd -= *distance;
    }
  }
}

void repairHunkEdgesOutOfPartiallyOwnedMacroExpansions(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    ArrayRef<int64_t> aToBMap, std::vector<diffutils::Hunk> &hunks) {
  for (size_t index = 0; index < hunks.size(); ++index) {
    diffutils::Hunk &hunk = hunks[index];

    // Only a replacement is repaired here.  A pure insertion has no A tokens to
    // own an expansion, and a pure deletion would have to grow a B side to be
    // widened, which changes the hunk's kind and belongs to the owner-aligned
    // deletion slide rather than to this local edge repair.
    if (hunk.aStart >= hunk.aEnd || hunk.bStart >= hunk.bEnd)
      continue;

    // Left edge: advance past the expansion it sits inside.  Each step gives
    // one leading token back to the untouched region on the left, so the token
    // must be identical on both sides for that region to reproduce it.
    const uint64_t originalAStart = hunk.aStart;
    while (hunk.aStart < hunk.aEnd && hunk.bStart < hunk.bEnd) {
      const RefoldModel::PPSpan *span = macroExpansionStraddledAtEdge(
          model, hunk.aStart, hunk.aEnd, /*edgeIsLeft=*/true);
      if (!span)
        break;
      if (!aAndBTokensAreIdentical(aToks, bToks, hunk.aStart, hunk.bStart))
        break;
      ++hunk.aStart;
      ++hunk.bStart;
    }
    if (hunk.aStart != originalAStart)
      REFOLD_LOG_DEBUG(
          "plan/hunk-edge",
          "hunk #{0} A=[{1},{2}) starts inside a macro expansion it does not "
          "end in; retracting the left edge by {3} identical token(s) to A={4} "
          "so the expansion is left untouched",
          index, originalAStart, hunk.aEnd, hunk.aStart - originalAStart,
          hunk.aStart);

    // Right edge: retreat before the expansion, giving trailing tokens back to
    // the untouched region on the right under the same identity requirement.
    const uint64_t originalAEnd = hunk.aEnd;
    while (hunk.aStart < hunk.aEnd && hunk.bStart < hunk.bEnd) {
      const RefoldModel::PPSpan *span = macroExpansionStraddledAtEdge(
          model, hunk.aEnd, hunk.aStart, /*edgeIsLeft=*/false);
      if (!span)
        break;
      if (!aAndBTokensAreIdentical(aToks, bToks, hunk.aEnd - 1,
                                   hunk.bEnd - 1)) {
        break;
      }
      --hunk.aEnd;
      --hunk.bEnd;
    }
    if (hunk.aEnd != originalAEnd)
      REFOLD_LOG_DEBUG(
          "plan/hunk-edge",
          "hunk #{0} A=[{1},{2}) ends inside a macro expansion it does not "
          "begin in; retracting the right edge by {3} identical token(s) to "
          "A={4} so the expansion is left untouched",
          index, hunk.aStart, originalAEnd, originalAEnd - hunk.aEnd,
          hunk.aEnd);

    // When the edge inside the expansion could not retract, the opposite edge
    // may: handing the identical tokens outside the expansion back leaves the
    // hunk contained in it.  This is still retraction -- it restores matches
    // and keeps the invocation -- so it is tried before widening consumes it.
    if (std::optional<uint64_t> distance =
            leftEdgeContainDistanceIntoSplitExpansion(model, aToks, bToks,
                                                      hunk);
        distance && *distance != 0) {
      REFOLD_LOG_DEBUG(
          "plan/hunk-edge",
          "hunk #{0} A=[{1},{2}) ends inside a macro expansion it does not "
          "begin in; retracting the left edge by {3} identical token(s) to "
          "A={4} so the hunk lies inside the expansion",
          index, hunk.aStart, hunk.aEnd, *distance, hunk.aStart + *distance);
      hunk.aStart += *distance;
      hunk.bStart += *distance;
    }
    if (std::optional<uint64_t> distance =
            rightEdgeContainDistanceIntoSplitExpansion(model, aToks, bToks,
                                                       hunk);
        distance && *distance != 0) {
      REFOLD_LOG_DEBUG(
          "plan/hunk-edge",
          "hunk #{0} A=[{1},{2}) begins inside a macro expansion it does not "
          "end in; retracting the right edge by {3} identical token(s) to "
          "A={4} so the hunk lies inside the expansion",
          index, hunk.aStart, hunk.aEnd, *distance, hunk.aEnd - *distance);
      hunk.aEnd -= *distance;
      hunk.bEnd -= *distance;
    }

    // Retraction restores a match the certifier left unforced, so it is tried
    // first: it keeps the invocation preserved.  It is unavailable when the
    // tokens at the edge are not the same on both sides, which is what an edit
    // that rewrites the expression around the callsite produces.  Widen instead
    // -- the expansion is then wholly consumed by one replacement, which is
    // realizable, where a split expansion is not.
    //
    // The neighbouring hunks bound each walk.  A walk that reaches a
    // neighbour while still inside the expansion merges the neighbour, which
    // holds part of the same expansion, and walks again from the merged edge;
    // each merge removes one hunk, so the loops terminate.  An edge that cannot
    // reach a whole-expansion boundary this way is left alone for the ordinary
    // realizer lattice, which refuses a partial cover.
    //
    // A left merge erases the current hunk, so the widening below indexes
    // `hunks` rather than using `hunk`.
    for (;;) {
      const bool floorIsHunkEdge = index != 0;
      const uint64_t aFloor = floorIsHunkEdge ? hunks[index - 1].aEnd : 0;
      const uint64_t bFloor = floorIsHunkEdge ? hunks[index - 1].bEnd : 0;
      std::optional<SplitExpansionWidening> widening =
          leftEdgeWidenDistanceOutOfSplitExpansion(model, aToks, bToks, aToBMap,
                                                   hunks[index], aFloor, bFloor,
                                                   floorIsHunkEdge);
      if (!widening)
        break;
      diffutils::Hunk &current = hunks[index];
      if (!widening->reachesNeighbour) {
        if (widening->distance != 0) {
          REFOLD_LOG_DEBUG(
              "plan/hunk-edge",
              "hunk #{0} A=[{1},{2}) starts inside a macro expansion whose "
              "tokens differ from B's; widening the left edge by {3} token(s) "
              "to A={4} so the expansion is wholly replaced",
              index, current.aStart, current.aEnd, widening->distance,
              current.aStart - widening->distance);
        }
        current.aStart -= widening->distance;
        current.bStart -= widening->distance;
        break;
      }
      diffutils::Hunk &previous = hunks[index - 1];
      REFOLD_LOG_DEBUG(
          "plan/hunk-edge",
          "hunk #{0} A=[{1},{2}) starts inside a macro expansion shared with "
          "hunk #{3} A=[{4},{5}); widening the left edge by {6} token(s) "
          "merges the two into A=[{4},{2}) so the expansion is wholly replaced",
          index, current.aStart, current.aEnd, index - 1, previous.aStart,
          previous.aEnd, widening->distance);
      previous.aEnd = current.aEnd;
      previous.bEnd = current.bEnd;
      hunks.erase(hunks.begin() + static_cast<std::ptrdiff_t>(index));
      --index;
    }

    for (;;) {
      const bool limitIsHunkEdge = index + 1 < hunks.size();
      const uint64_t aLimit = limitIsHunkEdge
                                  ? hunks[index + 1].aStart
                                  : static_cast<uint64_t>(aToks.size());
      const uint64_t bLimit = limitIsHunkEdge
                                  ? hunks[index + 1].bStart
                                  : static_cast<uint64_t>(bToks.size());
      std::optional<SplitExpansionWidening> widening =
          rightEdgeWidenDistanceOutOfSplitExpansion(
              model, aToks, bToks, aToBMap, hunks[index], aLimit, bLimit,
              limitIsHunkEdge);
      if (!widening)
        break;
      diffutils::Hunk &current = hunks[index];
      if (!widening->reachesNeighbour) {
        if (widening->distance != 0) {
          REFOLD_LOG_DEBUG(
              "plan/hunk-edge",
              "hunk #{0} A=[{1},{2}) ends inside a macro expansion whose tokens "
              "differ from B's; widening the right edge by {3} token(s) to "
              "A={4} so the expansion is wholly replaced",
              index, current.aStart, current.aEnd, widening->distance,
              current.aEnd + widening->distance);
        }
        current.aEnd += widening->distance;
        current.bEnd += widening->distance;
        break;
      }
      const diffutils::Hunk next = hunks[index + 1];
      REFOLD_LOG_DEBUG(
          "plan/hunk-edge",
          "hunk #{0} A=[{1},{2}) ends inside a macro expansion shared with "
          "hunk #{3} A=[{4},{5}); widening the right edge by {6} token(s) "
          "merges the two into A=[{1},{5}) so the expansion is wholly replaced",
          index, current.aStart, current.aEnd, index + 1, next.aStart,
          next.aEnd, widening->distance);
      current.aEnd = next.aEnd;
      current.bEnd = next.bEnd;
      hunks.erase(hunks.begin() + static_cast<std::ptrdiff_t>(index + 1));
    }
  }
}

} // namespace refold
} // namespace clang

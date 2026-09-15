//===--- RefoldTUEditPlanner.cpp --------------------------------*- C++ -*-===//
//
// Translation-unit edit planning service.
//
// TU-side planning lives here: provable insertion anchors, TU byte-span
// planning, pure-insertion include-boundary ownership, direct-TU hunk edit
// plans, and the closed trailing macro-call suffix extension policy.  Final
// TextEdit ordering/application remains outside this service.
//
//===----------------------------------------------------------------------===//

#include "edit/RefoldTUEditPlanner.h"

#include "edit/RefoldTUAnchorProof.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "model/RefoldModel.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <limits>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldTUEditPlanner::RefoldTUEditPlanner(Deps deps) : deps_(std::move(deps)) {}

std::optional<BoundaryParentIncludePlan>
RefoldTUEditPlanner::FindBoundaryParentIncludeForPureInsertion(
    const diffutils::Hunk &h) const {
  // This helper only applies to a pure insertion: the hunk must consume no
  // A-side tokens, but it must insert at least one B-side token.
  if (!h.isInsertOnly()) {
    return std::nullopt;
  }

  const uint64_t aPos = h.aStart;

  // We deliberately avoid "nearest token" probing here. A pure insertion is
  // attributed to an include only when the PP gap lands exactly on a recorded
  // include boundary.
  //
  // Collect the narrowest include ending at this gap (immediately on the left)
  // and the narrowest include beginning at this gap (immediately on the right).
  // Preferring the narrowest match lets an exact nested boundary beat any
  // enclosing include that shares the same endpoint.
  const RefoldModel::IncludeItem *leftBest = nullptr;
  uint64_t leftWidth = std::numeric_limits<uint64_t>::max();

  const RefoldModel::IncludeItem *rightBest = nullptr;
  uint64_t rightWidth = std::numeric_limits<uint64_t>::max();

  for (const auto &inc : deps_.model.GetIncludes()) {
    if (!inc.cover.IsValid())
      continue;

    const uint64_t width = inc.cover.end - inc.cover.begin;

    // Include immediately to the left of the insertion gap.
    if (inc.cover.end == aPos) {
      if (width < leftWidth) {
        leftBest = &inc;
        leftWidth = width;
      }
    }

    // Include immediately to the right of the insertion gap.
    if (inc.cover.begin == aPos) {
      if (width < rightWidth) {
        rightBest = &inc;
        rightWidth = width;
      }
    }
  }

  const std::optional<uint64_t> leftIncId =
      leftBest ? std::optional<uint64_t>(leftBest->id) : std::nullopt;
  const std::optional<uint64_t> rightIncId =
      rightBest ? std::optional<uint64_t>(rightBest->id) : std::nullopt;

  // If neither side hits an exact include boundary, this insertion cannot be
  // attributed to an include via boundary ownership.
  if (!leftIncId && !rightIncId)
    return std::nullopt;

  // When the gap sits between two include boundaries, attribute it to the
  // structural parent shared by the left and right side. This handles both
  // "between siblings" and "at one side only" cases uniformly.
  const std::optional<uint64_t> parentId =
      deps_.model.LeastCommonAncestorInclude(leftIncId, rightIncId);
  if (!parentId)
    return std::nullopt;

  const RefoldModel::IncludeItem *parent =
      deps_.model.GetIncludeById(*parentId);
  if (!parent)
    return std::nullopt;

  return BoundaryParentIncludePlan(aPos, parent->id, parent->parent);
}

bool RefoldTUEditPlanner::TUReplacementExtensionIsBTokenClosed(
    uint64_t aTokStart, uint64_t oldEnd, uint64_t extEnd, uint64_t bStart,
    uint64_t bEnd, StringRef tuPath) const {
  if (extEnd <= oldEnd)
    return true;
  if (deps_.abTokenMapA2B.empty())
    return false;

  const uint64_t tokCount = static_cast<uint64_t>(deps_.aTokens.size());
  for (uint64_t aTok = std::min(aTokStart, tokCount); aTok < tokCount; ++aTok) {
    std::optional<TUByteSpanPlan> span =
        deps_.tuAnchorProof.PlanTUByteSpan(aTok, aTok + 1, tuPath);
    if (!span)
      continue;

    if (span->tuByteEnd <= oldEnd)
      continue;
    if (span->tuByteBegin >= extEnd)
      break;

    // A partial-token overlap would mean the byte extension cut through an
    // A token. There is no token-closure proof for that shape, so preserve
    // the suffix rather than widening the edit.
    if (span->tuByteBegin < oldEnd || extEnd < span->tuByteEnd)
      return false;

    if (aTok >= static_cast<uint64_t>(deps_.abTokenMapA2B.size()))
      return false;

    const int64_t mappedB = deps_.abTokenMapA2B[static_cast<size_t>(aTok)];
    if (mappedB < 0)
      continue;

    if (static_cast<uint64_t>(mappedB) < bStart ||
        static_cast<uint64_t>(mappedB) >= bEnd)
      return false;
  }

  return true;
}

std::optional<TUTrailingCallSuffixExtension>
RefoldTUEditPlanner::MaybeExtendTUSpanOverClosedTrailingCallSuffix(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    StringRef replacement, const TUByteSpanPlan &initialSpan) const {
  if (initialSpan.tuByteBegin >= initialSpan.tuByteEnd)
    return std::nullopt;

  const uint64_t oldEnd = initialSpan.tuByteEnd;
  const uint64_t extEnd = extendChainedCallEnd(tuBytes, oldEnd, replacement);

  // Closed trailing call-suffix extension is not ordinary byte-span widening.
  // It is allowed only when the B-token suffix is independently closed and does
  // not require macro replay proof from the TU replacement itself. Keep this
  // policy isolated so TU edit construction cannot accidentally widen spans.
  const bool closed =
      extEnd != oldEnd && TUReplacementExtensionIsBTokenClosed(
                              h.aEnd, oldEnd, extEnd, h.bStart, h.bEnd, tuPath);
  if (!closed || !deps_.tuAnchorProof.ValidateOrdinaryDirectTUEnvelope(
                     tuPath, oldEnd, extEnd))
    return std::nullopt;

  return TUTrailingCallSuffixExtension(h.aEnd, h.aEnd, oldEnd, extEnd, h.bStart,
                                       h.bEnd,
                                       /*bTokenSuffixClosed=*/true);
}

void RefoldTUEditPlanner::MaybeExtendTUSpanOverClosedTrailingCallSuffix(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    StringRef replacement, std::pair<uint64_t, uint64_t> &span) const {
  TUByteSpanPlan initialSpan(h.aStart, h.aEnd, span.first, span.second);
  if (auto extension = MaybeExtendTUSpanOverClosedTrailingCallSuffix(
          h, tuPath, tuBytes, replacement, initialSpan))
    span.second = extension->extendedTUByteEnd;
}

std::optional<DirectTUHunkEditPlan>
RefoldTUEditPlanner::BuildDirectTUHunkEditPlan(
    const diffutils::Hunk &h, uint64_t hunkIndex,
    const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
    StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
    std::optional<uint64_t> materializedBByteBegin,
    std::optional<uint64_t> materializedBByteEnd,
    AcceptedPathKind acceptedPath,
    std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment) const {
  // Re-prove the original A-token carrier at the last direct-plan
  // construction boundary. Callers may legitimately widen the already-proved
  // raw range through a specialized suffix/separator theorem, but they may not
  // manufacture a different raw carrier or let the final physical edit cross
  // preprocessing structure. Pure insertions retain their exact base anchor;
  // movement past source #line prefixes requires the typed adjustment witness.
  std::optional<TUByteSpanPlan> rawSpan = deps_.tuAnchorProof.PlanTUByteSpan(
      h.aStart, h.aEnd, deps_.model.GetSourcePath());
  if (!rawSpan || rawSpan->tuByteBegin != rawTUStart ||
      rawSpan->tuByteEnd != rawTUEnd || span.second < span.first) {
    return std::nullopt;
  }

  TUByteSpanPlan spanPlan(h.aStart, h.aEnd, span.first, span.second,
                          rawSpan->insertionAnchor,
                          std::move(insertionAnchorAdjustment));
  if (!deps_.tuAnchorProof.ValidateTUOwnerRealizationCarrier(h, spanPlan))
    return std::nullopt;

  // Direct-TU hunk planning records the already-proved TU byte range and all
  // hunk-local attribution needed by the eventual TextEdit. It intentionally
  // does not certify accepted-result carriers or participate in final edit
  // ordering; those remain the assembler/audit boundary's responsibility.
  return DirectTUHunkEditPlan(h, hunkIndex, std::move(spanPlan),
                              std::optional<ResyncOutcome>(std::move(resync)),
                              acceptedPayload.str(), rawTUStart, rawTUEnd,
                              materializedBByteBegin, materializedBByteEnd,
                              acceptedPath);
}

bool maybeAdvanceTUInsertionPastSourceLineControlPrefix(
    const RefoldTUAnchorProof &tuAnchorProof,
    const RefoldLineControlProof &lineControlProof, const diffutils::Hunk &h,
    StringRef tuPath, StringRef tuBytes, std::pair<uint64_t, uint64_t> &span) {
  if (!h.isInsertOnly() || span.first != span.second ||
      tuAnchorProof.IsPPGapAtSelectedConditionalArmExit(h.aStart))
    return false;

  std::optional<uint64_t> exactAnchor =
      tuAnchorProof.AnchorToExactSlotBoundaryFromPPGap(tuPath, h.aStart);
  if (!exactAnchor || *exactAnchor != span.first)
    return false;

  std::optional<uint64_t> advancedAnchor =
      lineControlProof.AdvanceInsertionAnchorPastSourceLineControlPrefix(
          tuPath, std::nullopt, tuBytes, span.first);
  if (!advancedAnchor)
    return false;

  span.first = *advancedAnchor;
  span.second = *advancedAnchor;
  return true;
}

bool RefoldTUEditPlanner::IsOwnerUnresolvedNoTUAnchorOutOfDomain(
    const diffutils::Hunk &h, StringRef tuPath, const Owner &owner,
    bool mapsToTU) const {
  // This is the explicit out-of-domain predicate for hunks whose owner could
  // not be resolved and whose TU-level fallback witnesses are also unavailable.
  // It must remain derived-only: by the time this helper returns true, all
  // deterministic owner, include-boundary, TU-anchor, and TU-byte-span searches
  // have already failed. Do not infer ownership from proximity here.
  if (owner.kind != OwnerKind::Unknown)
    return false;

  if (h.isInsertOnly()) {
    // Pure insertions have extra deterministic witnesses: they may belong to a
    // parent include boundary, a provable TU insertion anchor, or a zero-width
    // TU byte span. Only if all of those are absent is the insertion outside
    // the declared refolding domain.
    if (FindBoundaryParentIncludeForPureInsertion(h))
      return false;
    if (mapsToTU)
      return false;
    if (deps_.tuAnchorProof.FindProvableTUInsertionAnchor(h.aStart, tuPath))
      return false;
    if (deps_.tuAnchorProof.PlanTUByteSpan(h.aStart, h.aEnd, tuPath))
      return false;
    return true;
  }

  // Non-insertion hunks are in-domain if the token map or TU byte-span mapping
  // can still witness the affected range. Without either, there is no declared
  // theorem-facing owner or TU anchor for this hunk.
  if (mapsToTU)
    return false;
  if (deps_.tuAnchorProof.PlanTUByteSpan(h.aStart, h.aEnd, tuPath))
    return false;
  return true;
}

std::string RefoldTUEditPlanner::BuildOwnerUnresolvedNoTUAnchorDetail(
    size_t hunkIndex, const diffutils::Hunk &h, StringRef tuPath,
    const Owner &owner, bool mapsToTU) const {
  const bool isInsertion = h.aStart == h.aEnd;

  // This detail string is emitted only after normal owner resolution has failed
  // to produce a macro/include/TU witness. Record those exhausted search spaces
  // explicitly so the terminal fallback explains the domain wall in proof
  // terms, not merely as `OwnerKind::Unknown`.
  // Ask whether any producer-recorded invocation covers this hunk rather than
  // asserting the search failed.  This was a hardcoded `true`, so the detail
  // reported an exhausted macro search that had never been run -- and a hunk
  // whose tail lies inside an expansion looked identical to one no macro comes
  // near.  When an invocation *is* named here, the hunk reached this refusal
  // with a macro owner available, which is a different defect from having none.
  const RefoldModel::MacroInvocation *coveringMacro =
      deps_.macroTopology.SmallestCoveringPatchableMacro(h.aStart, h.aEnd,
                                                         owner.includeId);
  const bool macroOwnerExhausted = coveringMacro == nullptr;
  const bool includeOwnerExhausted = owner.kind != OwnerKind::Include;

  // For pure insertions, check whether the insertion could still be explained
  // by an include-boundary parent. If present, this is a deterministic include
  // witness, not an out-of-domain owner-unresolved case.
  const RefoldModel::IncludeItem *boundaryInc = nullptr;
  if (isInsertion) {
    if (auto boundaryPlan = FindBoundaryParentIncludeForPureInsertion(h))
      boundaryInc = deps_.model.GetIncludeById(boundaryPlan->includeId);
  }
  const bool hasBoundaryInclude = boundaryInc != nullptr;

  // Also test the TU insertion-anchor path. A provable TU anchor keeps an
  // otherwise ownerless insertion inside the declared refolding domain.
  std::optional<TUInsertionAnchor> provableInsertionAnchor;
  if (isInsertion)
    provableInsertionAnchor =
        deps_.tuAnchorProof.FindProvableTUInsertionAnchor(h.aStart, tuPath);
  const bool hasProvableInsertionAnchor = provableInsertionAnchor.has_value();

  // Finally, check whether the hunk can be represented directly as a TU byte
  // span. This is the last generic TU witness before the edit is declared
  // outside the owner/TU-anchor domain.
  std::optional<TUByteSpanPlan> tuSpan =
      deps_.tuAnchorProof.PlanTUByteSpan(h.aStart, h.aEnd, tuPath);
  const bool hasTUByteSpan = tuSpan.has_value();
  const bool declaredDomainWall =
      IsOwnerUnresolvedNoTUAnchorOutOfDomain(h, tuPath, owner, mapsToTU);

  // These insertion-only fields make the diagnostic precise about which
  // insertion witnesses were attempted and whether each one was absent.
  std::string exactSlot = "n/a";
  std::string insertionAnchor = "n/a";
  std::string boundaryParent = "n/a";
  if (isInsertion) {
    if (auto slot = deps_.tuAnchorProof.FindExactSlotBoundaryFromPPGap(
            tuPath, h.aStart))
      exactSlot = llvm::formatv("{0}", *slot).str();
    else
      exactSlot = "none";

    if (provableInsertionAnchor)
      insertionAnchor =
          llvm::formatv("{0}", provableInsertionAnchor->tuByteOffset).str();
    else
      insertionAnchor = "none";

    if (boundaryInc)
      boundaryParent = llvm::formatv("inc#{0}", boundaryInc->id).str();
    else
      boundaryParent = "none";
  }

  const std::string tuSpanStr =
      tuSpan
          ? llvm::formatv("[{0},{1})", tuSpan->tuByteBegin, tuSpan->tuByteEnd)
                .str()
          : std::string("none");

  return llvm::formatv("edit #{0} A=[{1},{2}) insert={3} owner={4} "
                       "macroOwnerExhausted={5} "
                       "includeOwnerExhausted={6} mapsToTU={7} TUByteSpan={8} "
                       "hasTUByteSpan={9} "
                       "exactSlot={10} insertionAnchor={11} "
                       "hasProvableInsertionAnchor={12} "
                       "boundaryParentInclude={13} hasBoundaryInclude={14} "
                       "declaredDomainWall={15}",
                       hunkIndex, h.aStart, h.aEnd, isInsertion ? "yes" : "no",
                       owner.kind, macroOwnerExhausted ? "yes" : "no",
                       includeOwnerExhausted ? "yes" : "no",
                       mapsToTU ? "yes" : "no", tuSpanStr,
                       hasTUByteSpan ? "yes" : "no", exactSlot, insertionAnchor,
                       hasProvableInsertionAnchor ? "yes" : "no",
                       boundaryParent, hasBoundaryInclude ? "yes" : "no",
                       declaredDomainWall ? "yes" : "no")
      .str();
}

} // namespace refold
} // namespace clang

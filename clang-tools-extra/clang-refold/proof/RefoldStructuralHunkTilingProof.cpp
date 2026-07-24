//===--- RefoldStructuralHunkTilingProof.cpp --------------------*- C++ -*-===//
//
// Canonical proof predicates for durable structural hunk tilings.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldStructuralHunkTilingProof.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>

namespace clang {
namespace refold {
namespace {

/// Return whether the durable partition preserves a macro-state directive.
///
/// Producer-bound #define/#undef lines are owned by the specialized macro-state
/// repair theorem. Direct-TU span planning may discover those lines as evidence
/// but grants no protected-source authority. A Patch 3.1 boundary witness does
/// not compose with the repair theorem merely because its B boundaries are
/// unique, so such a witness must fail independently at every later proof
/// boundary as well as in the planner.
bool replacementPreservesMacroStateDirective(
    const StructuralHunkTilingWitness &witness) {
  return llvm::any_of(
      witness.edges, [](const StructuralHunkTilingEdgeWitness &edge) {
        if (edge.kind != StructuralHunkTilingEdgeKind::StateGap ||
            !edge.protectedPreprocessingStructure ||
            !edge.protectedStructureIdentityRecorded ||
            edge.producerIdentityKind !=
                StructuralProducerIdentityKind::MacroDirective ||
            !edge.producerItemId) {
          return false;
        }
        return edge.protectedStructureKind ==
                   StructuralProtectedStructureKind::MacroDefine ||
               edge.protectedStructureKind ==
                   StructuralProtectedStructureKind::MacroUndef;
      });
}

/// Return whether one state-transition carrier is intentionally empty.
///
/// A `PreservedInPlace` edge is discharged by retaining its exact source bytes;
/// it must not simultaneously claim replay, reconstruction, widening, or a
/// separately modeled before/after transition. Keeping this check here makes
/// the no-replay property part of the durable theorem rather than an incidental
/// consequence of the current witness builder.
bool preservedGapCarriesNoReplayedState(
    const StateTransitionProof &transition) {
  return transition.before.Empty() && transition.after.Empty() &&
         transition.suffixWitnesses.empty() &&
         transition.wideningWitnesses.empty() && !transition.failure;
}

/// Return whether two durable source ranges name one persisted source surface.
///
/// Path identity has already been resolved by the planner before the witness is
/// emitted. The durable proof therefore requires the exact persisted path and
/// include occurrence to agree; accepting a second spelling here would recreate
/// path-equivalence authority without the service that proved it.
bool durableSourceSurfacesAgree(
    const StructuralHunkTilingEdgeWitness &left,
    const StructuralHunkTilingEdgeWitness &right) {
  return left.sourceByteRangeKnown && right.sourceByteRangeKnown &&
         left.sourcePath == right.sourcePath &&
         left.sourceIncludeId == right.sourceIncludeId;
}

/// Return whether two nonempty intervals overlap on one durable source surface.
bool durableSourceIntervalsOverlap(
    const StructuralHunkTilingEdgeWitness &left,
    const StructuralHunkTilingEdgeWitness &right) {
  return durableSourceSurfacesAgree(left, right) &&
         left.sourceBegin < right.sourceEnd &&
         right.sourceBegin < left.sourceEnd;
}

} // namespace

bool structuralReplacementRequiresBoundaryProjection(
    const StructuralHunkTilingWitness &witness) {
  if (witness.originalBStart >= witness.originalBEnd)
    return false;

  return witness.reason ==
             StructuralTilingReason::PreservedPreprocessingStructure ||
         witness.reason ==
             StructuralTilingReason::MixedRealizersAndPreservedStructure;
}

bool structuralPreservedSourceTopologyIsComplete(
    const StructuralHunkTilingWitness &witness) {
  const bool preservesStructure =
      witness.reason ==
          StructuralTilingReason::PreservedPreprocessingStructure ||
      witness.reason ==
          StructuralTilingReason::MixedRealizersAndPreservedStructure;
  if (!preservesStructure || witness.originalAStart >= witness.originalAEnd ||
      !witness.uniquePartition || !witness.targetTokenStreamComposed ||
      !witness.stateTransitionsComposed ||
      witness.stateGapCount == 0 || witness.protectedStructureGapCount == 0 ||
      witness.preservedInPlaceGapCount != witness.stateGapCount ||
      witness.preservedInPlaceGapCount < witness.protectedStructureGapCount ||
      !witness.sourceByteCoverComplete ||
      !witness.preservedGapSourceOrderProven ||
      !witness.preservedGapsDisjointFromTokenSegments ||
      !witness.preservedGapsDisjointFromEdits) {
    return false;
  }

  uint64_t expectedA = witness.originalAStart;
  uint64_t expectedB = witness.originalBStart;
  const StructuralHunkTilingEdgeWitness *previousToken = nullptr;
  llvm::SmallVector<const StructuralHunkTilingEdgeWitness *, 8> pendingGaps;
  llvm::SmallVector<const StructuralHunkTilingEdgeWitness *, 8> allGaps;
  llvm::SmallVector<const StructuralHunkTilingEdgeWitness *, 8> allTokens;
  uint32_t observedStateGaps = 0;
  uint32_t observedProtectedGaps = 0;
  uint32_t observedTokenSegments = 0;

  for (const StructuralHunkTilingEdgeWitness &edge : witness.edges) {
    if (edge.kind == StructuralHunkTilingEdgeKind::StateGap) {
      if (!previousToken || !edge.zeroTokenStateGap ||
          edge.gapDisposition != StructuralGapDisposition::PreservedInPlace ||
          !edge.sourceBytesPreservedUnchanged ||
          !edge.ownerClosureComplete || !edge.ownerIdentityKnown ||
          edge.ownerIdentity.kind == OwnerKind::Unknown ||
          !edge.sourceByteRangeKnown || edge.sourcePath.empty() ||
          edge.sourceEnd <= edge.sourceBegin || edge.aStart != expectedA ||
          edge.aEnd != expectedA || edge.bStart != expectedB ||
          edge.bEnd != expectedB ||
          !preservedGapCarriesNoReplayedState(edge.canonicalStateTransition) ||
          (edge.protectedPreprocessingStructure &&
           (!edge.protectedStructureIdentityRecorded ||
            edge.protectedStructureKind ==
                StructuralProtectedStructureKind::Unknown)) ||
          (!edge.protectedPreprocessingStructure &&
           (edge.protectedStructureIdentityRecorded ||
            edge.protectedStructureKind !=
                StructuralProtectedStructureKind::Unknown ||
            edge.producerIdentityKind !=
                StructuralProducerIdentityKind::None ||
            edge.producerItemId || edge.producerConditionalGroupId ||
            edge.producerConditionalArmId))) {
        return false;
      }

      if (!pendingGaps.empty()) {
        const StructuralHunkTilingEdgeWitness &priorGap =
            *pendingGaps.back();
        if (!durableSourceSurfacesAgree(priorGap, edge) ||
            edge.sourceBegin < priorGap.sourceEnd) {
          return false;
        }
      }

      pendingGaps.push_back(&edge);
      allGaps.push_back(&edge);
      ++observedStateGaps;
      if (edge.protectedPreprocessingStructure)
        ++observedProtectedGaps;
      continue;
    }

    if (edge.kind != StructuralHunkTilingEdgeKind::TokenSegment ||
        edge.zeroTokenStateGap ||
        edge.gapDisposition != StructuralGapDisposition::Unknown ||
        edge.protectedPreprocessingStructure ||
        edge.protectedStructureIdentityRecorded ||
        edge.protectedStructureKind !=
            StructuralProtectedStructureKind::Unknown ||
        edge.producerIdentityKind != StructuralProducerIdentityKind::None ||
        edge.producerItemId || edge.producerConditionalGroupId ||
        edge.producerConditionalArmId || edge.sourceBytesPreservedUnchanged ||
        !edge.ownerClosureComplete ||
        !edge.ownerIdentityKnown ||
        edge.ownerIdentity.kind == OwnerKind::Unknown ||
        !edge.sourceByteRangeKnown || edge.sourcePath.empty() ||
        edge.sourceEnd <= edge.sourceBegin || edge.aStart != expectedA ||
        edge.bStart != expectedB || edge.aEnd <= edge.aStart ||
        edge.bEnd < edge.bStart ||
        ((edge.bStart == edge.bEnd) != edge.allowEmptyBEnvelope) ||
        !edge.protectedStructurePreservedOutsideSegment) {
      return false;
    }

    if (!pendingGaps.empty()) {
      if (!previousToken ||
          !durableSourceSurfacesAgree(*previousToken, edge)) {
        return false;
      }

      uint64_t sourceCursor = previousToken->sourceEnd;
      for (const StructuralHunkTilingEdgeWitness *gap : pendingGaps) {
        if (!gap || !durableSourceSurfacesAgree(*previousToken, *gap) ||
            gap->sourceBegin < sourceCursor ||
            edge.sourceBegin < gap->sourceEnd) {
          return false;
        }
        sourceCursor = gap->sourceEnd;
      }
      pendingGaps.clear();
    }

    previousToken = &edge;
    allTokens.push_back(&edge);
    expectedA = edge.aEnd;
    expectedB = edge.bEnd;
    ++observedTokenSegments;
  }

  // A preserved interval must remain between two token-bearing fragments. A
  // leading or trailing gap has no two-sided source-order theorem.
  if (!pendingGaps.empty() || !previousToken ||
      expectedA != witness.originalAEnd ||
      expectedB != witness.originalBEnd ||
      observedStateGaps != witness.stateGapCount ||
      observedProtectedGaps != witness.protectedStructureGapCount ||
      observedTokenSegments != witness.tokenSegmentCount) {
    return false;
  }

  // Recheck global disjointness from the persisted ranges themselves. Local
  // predecessor/successor ordering is insufficient for a mixed-realizer path
  // that later revisits the same physical source occurrence.
  for (size_t gapIndex = 0; gapIndex < allGaps.size(); ++gapIndex) {
    for (size_t otherGapIndex = gapIndex + 1;
         otherGapIndex < allGaps.size(); ++otherGapIndex) {
      if (durableSourceIntervalsOverlap(*allGaps[gapIndex],
                                        *allGaps[otherGapIndex])) {
        return false;
      }
    }
    for (const StructuralHunkTilingEdgeWitness *token : allTokens) {
      if (!token ||
          durableSourceIntervalsOverlap(*allGaps[gapIndex], *token)) {
        return false;
      }
    }
  }

  return true;
}

bool structuralReplacementBoundaryProjectionIsComplete(
    const StructuralHunkTilingWitness &witness) {
  // Macro-state directive ordering remains under the dedicated liveness repair
  // theorem.  Rejecting it here prevents a stale, forged, or externally
  // reconstructed structural witness from bypassing the planner-level gate.
  if (replacementPreservesMacroStateDirective(witness))
    return false;

  if (!structuralPreservedSourceTopologyIsComplete(witness))
    return false;

  if (witness.originalAStart >= witness.originalAEnd ||
      witness.originalBStart >= witness.originalBEnd ||
      !witness.uniquePartition || !witness.targetTokenStreamComposed ||
      !witness.uniqueBoundaryProjectionProven ||
      !witness.physicalSourceRunsProven ||
      witness.physicalSourceRunCount < 2 ||
      witness.tokenSegmentCount < witness.physicalSourceRunCount ||
      witness.boundaryProjectionCount != witness.boundaryProjections.size() ||
      witness.boundaryProjectionCount + 1 != witness.physicalSourceRunCount) {
    return false;
  }

  // A pure preserved-structure partition has no owner boundary that could
  // require additional token segments inside one canonical physical run.  It
  // must therefore retain the Patch 3.1 minimum-fragment theorem.  A combined
  // mixed-realizer partition may legitimately contain more token segments than
  // physical runs; its unique lowest-cost DP path is recorded by
  // `uniquePartition` and the protected seams are still validated below.
  if (witness.reason ==
          StructuralTilingReason::PreservedPreprocessingStructure &&
      (!witness.uniqueMinimumFragmentPartition ||
       witness.physicalSourceRunCount != witness.tokenSegmentCount)) {
    return false;
  }

  uint64_t expectedA = witness.originalAStart;
  uint64_t expectedB = witness.originalBStart;
  const StructuralHunkTilingEdgeWitness *previousToken = nullptr;
  bool protectedStructureSincePreviousToken = false;
  size_t projectionIndex = 0;
  uint32_t observedTokenCount = 0;

  for (const StructuralHunkTilingEdgeWitness &edge : witness.edges) {
    if (edge.kind == StructuralHunkTilingEdgeKind::StateGap) {
      // A preserved source gap must be bracketed by emitted token carriers.
      // Leading or trailing gaps have no two-sided edit-disjointness theorem and
      // cannot consume a replacement-boundary projection record.
      if (!previousToken || edge.aStart != expectedA ||
          edge.aEnd != expectedA || edge.bStart != expectedB ||
          edge.bEnd != expectedB) {
        return false;
      }
      protectedStructureSincePreviousToken |=
          edge.protectedPreprocessingStructure;
      continue;
    }
    if (edge.kind != StructuralHunkTilingEdgeKind::TokenSegment ||
        edge.aStart != expectedA || edge.bStart != expectedB ||
        edge.aEnd <= edge.aStart || edge.bEnd < edge.bStart ||
        ((edge.bStart == edge.bEnd) != edge.allowEmptyBEnvelope)) {
      return false;
    }

    if (previousToken && protectedStructureSincePreviousToken) {
      if (projectionIndex >= witness.boundaryProjections.size())
        return false;

      const StructuralBoundaryProjectionWitness &projection =
          witness.boundaryProjections[projectionIndex++];
      if (!projection.uniqueProjection ||
          projection.lowerBTokenBoundary !=
              projection.upperBTokenBoundary ||
          projection.bTokenBoundary != projection.lowerBTokenBoundary ||
          projection.aTokenBoundary != previousToken->aEnd ||
          projection.aTokenBoundary != edge.aStart ||
          projection.bTokenBoundary != previousToken->bEnd ||
          projection.bTokenBoundary != edge.bStart) {
        return false;
      }
    }

    previousToken = &edge;
    protectedStructureSincePreviousToken = false;
    expectedA = edge.aEnd;
    expectedB = edge.bEnd;
    ++observedTokenCount;
  }

  return !protectedStructureSincePreviousToken &&
         observedTokenCount == witness.tokenSegmentCount &&
         projectionIndex == witness.boundaryProjections.size() &&
         expectedA == witness.originalAEnd && expectedB == witness.originalBEnd;
}

} // namespace refold
} // namespace clang

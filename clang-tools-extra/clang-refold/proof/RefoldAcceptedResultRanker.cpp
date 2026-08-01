//===--- RefoldAcceptedResultRanker.cpp -------------------------*- C++ -*-===//
//
// Implementation of the accepted-result ranking and selection service.  See
// the header for the architectural contract; the per-method comments below
// document the rule each step enforces.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldAcceptedResultRanker.h"

#include "proof/RefoldAcceptedResultPredicates.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldWitnessResolver.h"
#include "proof/RefoldWitnessTrace.h"

#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldAcceptedResultRanker::RefoldAcceptedResultRanker(Dependencies deps)
    : deps_(std::move(deps)) {}

bool RefoldAcceptedResultRanker::LatticePrefers(const ProofSummary &lhs,
                                                const ProofSummary &rhs) const {
  auto theoremTieBreakerPrefers =
      [](const ProofSummary &candidate,
         const ProofSummary &other) -> std::optional<bool> {
    // Explicit named theorem tie-breakers are the only permitted way to express
    // source-shape preferences that are not derivable from the generic summary
    // ranks below.  They do not discharge proof obligations by themselves; they
    // only order two already-normalized accepted summaries after the builder
    // has proved the witness-specific preconditions for the named rule.
    switch (candidate.selectionTieBreaker) {
    case TheoremSelectionTieBreakerKind::
        ExactTUArgumentEditOverEquivalentMacroArgsOnly:
      return other.inventory.currentPath ==
             AcceptedPathKind::MacroArgsOnlyStandard;
    case TheoremSelectionTieBreakerKind::Unknown:
      return std::nullopt;
    }
    return std::nullopt;
  };

  if (std::optional<bool> lhsTie = theoremTieBreakerPrefers(lhs, rhs)) {
    if (*lhsTie)
      return true;
  }
  if (std::optional<bool> rhsTie = theoremTieBreakerPrefers(rhs, lhs)) {
    if (*rhsTie)
      return false;
  }

  auto preferenceRank = [](SelectionPreference preference) -> uint8_t {
    // Lower rank means stronger selection preference. These are lattice-level
    // policy categories, not local heuristics.
    switch (preference) {
    case SelectionPreference::PreferExactAnchoring:
      return 0;
    case SelectionPreference::PreferStructurePreservation:
      return 1;
    case SelectionPreference::PreferSurfaceRealization:
      return 2;
    case SelectionPreference::Unknown:
      return 3;
    }
    return 3;
  };

  auto surfaceDispositionRank = [](SurfaceDisposition disposition) -> uint8_t {
    // When two proofs have the same structural preference, prefer the result
    // that stays closer to the original source structure before falling back to
    // broader surface/TU emission.
    switch (disposition) {
    case SurfaceDisposition::None:
      return 0;
    case SurfaceDisposition::RealizeWholeCoverMacros:
      return 1;
    case SurfaceDisposition::RealizeInlineTouchedIncludesFromB:
      return 2;
    case SurfaceDisposition::RealizeMaterializedIncludeExpansion:
      return 3;
    case SurfaceDisposition::RealizeTranslationUnitByteEdit:
      return 4;
    case SurfaceDisposition::EmitEditedPreprocessedStream:
      return 5;
    }
    return 5;
  };

  const uint8_t lhsPreference = preferenceRank(lhs.preference);
  const uint8_t rhsPreference = preferenceRank(rhs.preference);
  if (lhsPreference != rhsPreference)
    return lhsPreference < rhsPreference;

  const uint8_t lhsSurfaceDisposition =
      surfaceDispositionRank(lhs.surfaceDisposition);
  const uint8_t rhsSurfaceDisposition =
      surfaceDispositionRank(rhs.surfaceDisposition);
  if (lhsSurfaceDisposition != rhsSurfaceDisposition)
    return lhsSurfaceDisposition < rhsSurfaceDisposition;

  auto hasMixedOwnerTilingProof = [](const ProofSummary &summary) {
    return summary.hasMixedOwnerTilingWitness &&
           summary.theoremClass == TheoremProofClass::MixedOwnerTilingProof;
  };

  auto mixedOwnerCoverWidth = [](const MixedOwnerTilingWitness &witness) {
    return (witness.originalAEnd - witness.originalAStart) +
           (witness.originalBEnd - witness.originalBStart);
  };

  const bool lhsMixedOwner = hasMixedOwnerTilingProof(lhs);
  const bool rhsMixedOwner = hasMixedOwnerTilingProof(rhs);
  if (lhsMixedOwner != rhsMixedOwner &&
      lhs.inventory.currentPath == rhs.inventory.currentPath) {
    // A segment proven by a durable mixed-owner tiling is strictly stronger
    // than the owner-specific realization/preservation summary for the same
    // emitted artifact, but that ordering belongs here in the lattice rather
    // than in the path-local witness attachment code.
    return lhsMixedOwner;
  }

  if (lhsMixedOwner && rhsMixedOwner) {
    const MixedOwnerTilingWitness &lhsWitness = lhs.mixedOwnerTilingWitness;
    const MixedOwnerTilingWitness &rhsWitness = rhs.mixedOwnerTilingWitness;
    const uint64_t lhsWidth = mixedOwnerCoverWidth(lhsWitness);
    const uint64_t rhsWidth = mixedOwnerCoverWidth(rhsWitness);
    if (lhsWidth != rhsWidth)
      return lhsWidth < rhsWidth;
    if (lhsWitness.tokenSegmentCount != rhsWitness.tokenSegmentCount)
      return lhsWitness.tokenSegmentCount > rhsWitness.tokenSegmentCount;
    if (lhsWitness.stateGapCount != rhsWitness.stateGapCount)
      return lhsWitness.stateGapCount > rhsWitness.stateGapCount;
    if (lhsWitness.witnessId != rhsWitness.witnessId)
      return lhsWitness.witnessId > rhsWitness.witnessId;
  }

  // The remaining tie-breakers are deterministic enum orderings. They should
  // only be reached after the explicit lattice preferences above agree.  Use
  // the final theorem class here; AcceptedProofClass remains available only as
  // construction provenance and no longer orders selectable proofs.
  if (lhs.theoremClass != rhs.theoremClass)
    return static_cast<uint8_t>(lhs.theoremClass) <
           static_cast<uint8_t>(rhs.theoremClass);

  return static_cast<uint8_t>(lhs.inventory.currentPath) <
         static_cast<uint8_t>(rhs.inventory.currentPath);
}

bool RefoldAcceptedResultRanker::IsSelectableAcceptedResultCandidate(
    const AcceptedResultCandidate &candidate) const {
  // Selection uses the same theorem-normalization gate as final emission.  A
  // path-specific builder may still certify AcceptedPathKind and
  // AcceptedProofClass, but those are construction provenance only; a selector
  // may consider the result only after NormalizeAcceptedProof() proves that the
  // candidate maps to one final theorem class.
  return deps_.normalizeAcceptedProof(candidate).has_value();
}

bool RefoldAcceptedResultRanker::AcceptedResultCandidateProofPrefers(
    const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const {
  if (LatticePrefers(lhs.proofSummary, rhs.proofSummary))
    return true;
  if (LatticePrefers(rhs.proofSummary, lhs.proofSummary))
    return false;
  return false;
}

bool RefoldAcceptedResultRanker::AcceptedResultCandidateCanonicalPrefers(
    const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const {
  // The lattice intentionally stays coarse. When two summaries tie, prefer the
  // candidate that is more specific about the concrete artifact it will emit.
  // This is a named canonical preference step rather than a proof validity
  // test.  It is reached only after both candidates are selectable and neither
  // proof summary strictly outranks the other.
  if (lhs.kind != rhs.kind)
    return static_cast<uint8_t>(lhs.kind) < static_cast<uint8_t>(rhs.kind);

  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  if (lhs.end != rhs.end)
    return lhs.end < rhs.end;

  if (lhs.hasRootMacroId != rhs.hasRootMacroId)
    return lhs.hasRootMacroId;
  if (lhs.hasRootMacroId && lhs.rootMacroId != rhs.rootMacroId)
    return lhs.rootMacroId < rhs.rootMacroId;

  if (lhs.hasOwnerIncludeId != rhs.hasOwnerIncludeId)
    return lhs.hasOwnerIncludeId;
  if (lhs.hasOwnerIncludeId && lhs.ownerIncludeId != rhs.ownerIncludeId)
    return lhs.ownerIncludeId < rhs.ownerIncludeId;

  if (lhs.hasAnchorByte != rhs.hasAnchorByte)
    return lhs.hasAnchorByte;
  if (lhs.hasAnchorByte && lhs.anchorByte != rhs.anchorByte)
    return lhs.anchorByte < rhs.anchorByte;

  return false;
}

bool RefoldAcceptedResultRanker::AcceptedResultCandidatePrefers(
    const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const {
  if (AcceptedResultCandidateProofPrefers(lhs, rhs))
    return true;
  if (AcceptedResultCandidateProofPrefers(rhs, lhs))
    return false;
  return AcceptedResultCandidateCanonicalPrefers(lhs, rhs);
}

std::optional<size_t> RefoldAcceptedResultRanker::SelectPreferredCandidateIndex(
    size_t candidateCount, function_ref<bool(size_t)> isSelectable,
    function_ref<bool(size_t, size_t)> prefers) const {
  SmallVector<size_t, 8> selectableIndices;

  // This keeps selector behavior stable while explicitly separating the two
  // selector responsibilities:
  //
  //   1. proof validity: which candidates are selectable at all?
  //   2. canonical preference: among those already-valid candidates, which
  //      deterministic representative should the selector choose?
  //
  // The preference relation is deliberately never invoked on an invalid
  // candidate. Equivalence-class canonicalization can replace the second step
  // without changing the validity gate.
  for (size_t i = 0; i < candidateCount; ++i)
    if (isSelectable(i))
      selectableIndices.push_back(i);

  std::optional<size_t> bestIdx;
  for (size_t idx : selectableIndices) {
    if (!bestIdx) {
      bestIdx = idx;
      continue;
    }
    if (prefers(idx, *bestIdx))
      bestIdx = idx;
  }

  const uint64_t selectableCount = selectableIndices.size();
  if (selectableCount > 1) {
    ++deps_.lastTheoremAudit.selectorCompetitions;
    if (bestIdx)
      ++deps_.lastTheoremAudit.selectorResolutions;
  } else if (!bestIdx && candidateCount != 0) {
    ++deps_.lastTheoremAudit.selectorNoSelectable;
    if (candidateCount > 1)
      ++deps_.lastTheoremAudit.selectorUnresolvedCompetitions;
  }

  return bestIdx;
}

bool RefoldAcceptedResultRanker::IsSelectableMacroSelectionCandidate(
    const MacroSelectionCandidate &candidate) const {
  if (IsSelectableAcceptedResultCandidate(candidate.selectorCandidate))
    return true;

  // The only macro-local non-final selector proof admitted by this carrier is
  // the explicit nested proof-root exception.  The emitted candidate remains a
  // separate optional and is audited only if it is later certified for
  // emission.
  return candidate.selectorOnly &&
         AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
             candidate.selectorCandidate);
}

bool RefoldAcceptedResultRanker::MacroSelectionCandidatePrefers(
    const MacroSelectionCandidate &lhs,
    const MacroSelectionCandidate &rhs) const {
  return AcceptedResultCandidatePrefers(lhs.selectorCandidate,
                                        rhs.selectorCandidate);
}

std::optional<::clang::refold::SelectedMacroSelectionCandidate>
RefoldAcceptedResultRanker::SelectPreferredMacroSelectionCandidate(
    ArrayRef<MacroSelectionCandidate> candidates) const {
  auto isSelectable = [&](size_t idx) {
    return IsSelectableMacroSelectionCandidate(candidates[idx]);
  };
  auto prefers = [&](size_t lhsIdx, size_t rhsIdx) {
    return MacroSelectionCandidatePrefers(candidates[lhsIdx],
                                          candidates[rhsIdx]);
  };

  std::optional<size_t> legacyBestIdx =
      SelectPreferredCandidateIndex(candidates.size(), isSelectable, prefers);

  WitnessResolverDecision resolverDecision =
      deps_.witnessResolver.ResolveWitnessesForSelection(
          kSelectPreferredMacroSelectionRole, candidates.size(),
          isSelectable,
          [&](size_t idx) {
            return deps_.witnessResolver.BuildRefoldWitness(
                candidates[idx].selectorCandidate,
                kSelectPreferredMacroSelectionRole, idx);
          },
          prefers, legacyBestIdx);

  std::optional<size_t> bestIdx = legacyBestIdx;
  // Preserve the selector-only diagnostic trace that is not part of the generic
  // resolver validity predicate: a nested selector candidate may be useful for
  // ranking but still lack an emission-normalized carrier.
  if (deps_.witnessTrace.ShouldEmitProofLog()) {
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (!isSelectable(i) || !candidates[i].selectorOnly ||
          candidates[i].emittedCandidate) {
        continue;
      }
      deps_.witnessTrace.TraceWitnessRejected(
          deps_.witnessResolver.BuildRefoldWitness(
              candidates[i].selectorCandidate,
              kSelectPreferredMacroSelectionRole, i),
          WitnessRejectReason::SelectorOnlyNoEmittedCandidate,
          "selector-only macro witness has no emission-normalized carrier");
    }
  }

  if (resolverDecision.ShouldFailClosed())
    return std::nullopt;
  if (resolverDecision.ShouldUseResolverIndex())
    bestIdx = resolverDecision.resolverIndex;

  if (!bestIdx)
    return std::nullopt;

  SelectedMacroSelectionCandidate selected;
  selected.candidate = candidates[*bestIdx];
  selected.index = *bestIdx;
  deps_.witnessTrace.TraceWitnessChosen(
      deps_.witnessResolver.BuildRefoldWitness(
          selected.candidate.selectorCandidate,
          kSelectPreferredMacroSelectionRole, *bestIdx),
      *bestIdx);
  return selected;
}

std::optional<::clang::refold::SelectedAcceptedResultCandidate>
RefoldAcceptedResultRanker::SelectPreferredAcceptedResultCandidate(
    ArrayRef<AcceptedResultCandidate> candidates) const {
  auto isSelectable = [&](size_t idx) {
    return IsSelectableAcceptedResultCandidate(candidates[idx]);
  };
  auto prefers = [&](size_t lhsIdx, size_t rhsIdx) {
    return AcceptedResultCandidatePrefers(candidates[lhsIdx],
                                          candidates[rhsIdx]);
  };

  std::optional<size_t> legacyBestIdx =
      SelectPreferredCandidateIndex(candidates.size(), isSelectable, prefers);

  WitnessResolverDecision resolverDecision =
      deps_.witnessResolver.ResolveWitnessesForSelection(
          "SelectPreferredAcceptedResultCandidate", candidates.size(),
          isSelectable,
          [&](size_t idx) {
            return deps_.witnessResolver.BuildRefoldWitness(
                candidates[idx], "SelectPreferredAcceptedResultCandidate", idx);
          },
          prefers, legacyBestIdx);

  std::optional<size_t> bestIdx = legacyBestIdx;
  if (resolverDecision.ShouldFailClosed())
    return std::nullopt;
  if (resolverDecision.ShouldUseResolverIndex())
    bestIdx = resolverDecision.resolverIndex;

  if (!bestIdx)
    return std::nullopt;

  deps_.theoremAudit.AuditAcceptedResultCandidateForLegacyAuthority(
      candidates[*bestIdx], "SelectPreferredAcceptedResultCandidate");

  SelectedAcceptedResultCandidate selected;
  selected.candidate = candidates[*bestIdx];
  selected.index = *bestIdx;
  deps_.witnessTrace.TraceWitnessChosen(
      deps_.witnessResolver.BuildRefoldWitness(
          selected.candidate, "SelectPreferredAcceptedResultCandidate",
          *bestIdx),
      *bestIdx);
  return selected;
}

std::optional<size_t>
RefoldAcceptedResultRanker::SelectPreferredAcceptedResultCandidateIndex(
    ArrayRef<AcceptedResultCandidate> candidates) const {
  std::optional<SelectedAcceptedResultCandidate> selected =
      SelectPreferredAcceptedResultCandidate(candidates);
  if (!selected)
    return std::nullopt;
  return selected->index;
}

} // namespace refold
} // namespace clang

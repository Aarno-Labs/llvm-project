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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldAcceptedResultRanker::RefoldAcceptedResultRanker(Dependencies deps)
    : deps_(std::move(deps)) {}

namespace {

/// Return whether \p candidate names a theorem tie-breaker that applies
/// against \p other.
///
/// This is one direction of a named preference, not an order: it reads a
/// coordinate of \p other that no summary rank consults, so composing it with
/// the ranks would make the combined relation depend on which pair happens to
/// be compared first.  `NamedTheoremTieBreakerPrefers` calls it in both
/// directions and keeps only a decision the two directions agree on.
bool namedTieBreakerApplies(const ProofSummary &candidate,
                            const ProofSummary &other) {
  switch (candidate.selectionTieBreaker) {
  case TheoremSelectionTieBreakerKind::
      ExactTUArgumentEditOverEquivalentMacroArgsOnly:
    // An exact TU byte edit inside the original argument spelling preserves
    // callsite trivia that args-only macro reconstruction discards.  The
    // caller owns the obligation that the two spellings realize the same
    // edit; this predicate only recognizes the shape.
    return other.inventory.currentPath ==
           AcceptedPathKind::MacroArgsOnlyStandard;
  case TheoremSelectionTieBreakerKind::Unknown:
    return false;
  }
  return false;
}

} // namespace

std::optional<bool> RefoldAcceptedResultRanker::NamedTheoremTieBreakerPrefers(
    const ProofSummary &lhs, const ProofSummary &rhs) {
  const bool lhsNames = namedTieBreakerApplies(lhs, rhs);
  const bool rhsNames = namedTieBreakerApplies(rhs, lhs);
  // Both sides naming a preference against the other orders nothing, and
  // answering either way would break asymmetry.  Report that no named
  // tie-breaker decided, so the caller falls back to the proof order.
  if (lhsNames == rhsNames)
    return std::nullopt;
  return lhsNames;
}

bool RefoldAcceptedResultRanker::ProvenEquivalentArtifactPrefers(
    const ProofSummary &lhs, const ProofSummary &rhs) {
  if (std::optional<bool> named = NamedTheoremTieBreakerPrefers(lhs, rhs))
    return *named;
  return ProofDominates(lhs, rhs) && !ProofDominates(rhs, lhs);
}

bool RefoldAcceptedResultRanker::ProofDominates(const ProofSummary &lhs,
                                                const ProofSummary &rhs) {
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
  if (lhsMixedOwner != rhsMixedOwner) {
    // A segment proven by a durable mixed-owner tiling is strictly stronger
    // than the owner-specific realization/preservation summary it competes
    // with, and that ordering belongs here rather than in the path-local
    // witness attachment code.
    //
    // The rule used to apply only when the two summaries also agreed on
    // `inventory.currentPath`, which stood in for "the same emitted
    // artifact".  A comparison that switches on whether a third coordinate
    // matches is not transitive: with the guard in place, a mixed-owner
    // summary could beat a same-path competitor while losing to a
    // different-path one by the theorem-class fallback below, and the winner
    // of the resulting cycle depended on candidate push order.  The guard is
    // gone because the ranks above already restrict this comparison to
    // summaries in the same structural class, and a proof strength ordering
    // must not depend on which enum value labeled the builder that produced
    // the summary.
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
  // construction provenance and does not order selectable proofs.
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

ProofDominanceOrder
RefoldAcceptedResultRanker::CompareAcceptedResultCandidateProofs(
    const AcceptedResultCandidate &lhs, const AcceptedResultCandidate &rhs) {
  if (ProofDominates(lhs.proofSummary, rhs.proofSummary))
    return ProofDominanceOrder::LeftDominates;
  if (ProofDominates(rhs.proofSummary, lhs.proofSummary))
    return ProofDominanceOrder::RightDominates;
  return ProofDominanceOrder::Incomparable;
}

bool RefoldAcceptedResultRanker::AcceptedResultCandidateProofPrefers(
    const AcceptedResultCandidate &lhs, const AcceptedResultCandidate &rhs) {
  return CompareAcceptedResultCandidateProofs(lhs, rhs) ==
         ProofDominanceOrder::LeftDominates;
}

bool RefoldAcceptedResultRanker::AcceptedResultCandidateCanonicalPrefers(
    const AcceptedResultCandidate &lhs, const AcceptedResultCandidate &rhs) {
  // The proof order intentionally stays coarse. When two summaries tie, prefer
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
    const AcceptedResultCandidate &lhs, const AcceptedResultCandidate &rhs) {
  // Only incomparability reaches the canonical tie-break.  Asking the proof
  // order once, and acting on all three of its answers, is what keeps a
  // dominated candidate from being re-examined as if it had merely tied.
  switch (CompareAcceptedResultCandidateProofs(lhs, rhs)) {
  case ProofDominanceOrder::LeftDominates:
    return true;
  case ProofDominanceOrder::RightDominates:
    return false;
  case ProofDominanceOrder::Incomparable:
    break;
  }
  return AcceptedResultCandidateCanonicalPrefers(lhs, rhs);
}

std::optional<SelectionOrderViolation>
RefoldAcceptedResultRanker::FindSelectionOrderViolation(
    ArrayRef<size_t> indices, function_ref<bool(size_t, size_t)> prefers) {
  // Irreflexivity first: a candidate that outranks itself makes every later
  // answer meaningless, so report it before any pair or triple.
  for (size_t i = 0; i < indices.size(); ++i) {
    if (prefers(indices[i], indices[i])) {
      return SelectionOrderViolation{SelectionOrderLaw::Irreflexivity,
                                     indices[i], indices[i], indices[i]};
    }
  }

  for (size_t i = 0; i < indices.size(); ++i) {
    for (size_t j = i + 1; j < indices.size(); ++j) {
      if (prefers(indices[i], indices[j]) && prefers(indices[j], indices[i])) {
        return SelectionOrderViolation{SelectionOrderLaw::Asymmetry, indices[i],
                                       indices[j], indices[j]};
      }
    }
  }

  // Transitivity is the law a max scan actually consumes: it is what lets the
  // scan discard a candidate permanently after one comparison.
  for (size_t i = 0; i < indices.size(); ++i) {
    for (size_t j = 0; j < indices.size(); ++j) {
      if (i == j || !prefers(indices[i], indices[j]))
        continue;
      for (size_t k = 0; k < indices.size(); ++k) {
        if (k == i || k == j || !prefers(indices[j], indices[k]))
          continue;
        if (!prefers(indices[i], indices[k])) {
          return SelectionOrderViolation{SelectionOrderLaw::Transitivity,
                                         indices[i], indices[j], indices[k]};
        }
      }
    }
  }

  return std::nullopt;
}

bool RefoldAcceptedResultRanker::AuditSelectionOrder(
    StringRef role, ArrayRef<size_t> selectableIndices,
    function_ref<bool(size_t, size_t)> prefers) const {
  // With fewer than two selectable candidates the max scan never invokes the
  // relation, so no order defect can change the answer.
  if (selectableIndices.size() < 2)
    return false;
  if (!deps_.witnessTrace.ShouldEmitProofLog())
    return false;

  ++deps_.lastTheoremAudit.selectorOrderAudits;
  std::optional<SelectionOrderViolation> violation =
      FindSelectionOrderViolation(selectableIndices, prefers);
  if (!violation)
    return false;

  ++deps_.lastTheoremAudit.selectorOrderViolations;
  deps_.witnessTrace.TraceSelectionOrderViolation(role, *violation,
                                                  selectableIndices.size());
  // A violation is a defect in the preference relation, not in any candidate.
  // Strict mode refuses the competition rather than emitting a winner that a
  // different candidate push order would not have produced.
  return deps_.witnessTrace.GetWitnessResolverMode() ==
         WitnessResolverMode::Strict;
}

std::optional<size_t> RefoldAcceptedResultRanker::SelectPreferredCandidateIndex(
    StringRef role, size_t candidateCount,
    function_ref<bool(size_t)> isSelectable,
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

  // The scan below keeps a single running best and never reconsiders a
  // discarded candidate, so its winner is push-order independent only when the
  // supplied relation is a strict order.  Check that precondition before
  // relying on it; an unaudited competition is accounted the same way an
  // unresolved one is.
  if (AuditSelectionOrder(role, selectableIndices, prefers)) {
    ++deps_.lastTheoremAudit.selectorUnresolvedCompetitions;
    return std::nullopt;
  }

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
    const MacroSelectionCandidate &lhs, const MacroSelectionCandidate &rhs) {
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
      SelectPreferredCandidateIndex(kSelectPreferredMacroSelectionRole,
                                    candidates.size(), isSelectable, prefers);

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
      SelectPreferredCandidateIndex("SelectPreferredAcceptedResultCandidate",
                                    candidates.size(), isSelectable, prefers);

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

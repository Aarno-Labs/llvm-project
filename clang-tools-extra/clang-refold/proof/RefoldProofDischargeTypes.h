//===--- RefoldProofDischargeTypes.h ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Proof-discharge records and the canonical emitted proof for clang-refold.
//
// Local proof obligations, why one failed, and the accumulator that keeps that
// state deterministic.  `EmittedProof` is the canonical theorem-facing carrier
// assembled from the witnesses below it -- value-only and side-effect-free, so
// a `ProofSummary` can own one without the summary becoming the authority for
// its own validity.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFDISCHARGETYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFDISCHARGETYPES_H

#include "proof/RefoldAnchorWitnessTypes.h"
#include "proof/RefoldTheoremTypes.h"
#include "proof/RefoldTilingWitnessTypes.h"

#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>

namespace clang {
namespace refold {

using llvm::StringRef;

/// \brief Status produced when the engine evaluates a local proof contract.
///
/// The engine records explicit local obligations for every normalized proof
/// summary. Converted selector sites already use the discharge result as the
/// participation gate, while construction paths that still maintain local
/// facts mirror them into the same record so the theorem boundary stays
/// explicit.
#define REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X)                           \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(PendingMaterialization)                                             \
  REFOLD_X(Discharged)                                                         \
  REFOLD_X(Rejected)

enum class ProofDischargeStatus : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(ProofDischargeStatus value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case ProofDischargeStatus::name:                                             \
    return #name;
    REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_PROOF_DISCHARGE_STATUS_LIST

/// \brief Named local obligations used by proof-discharge records.
#define REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X)                            \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(AcceptedPathClassified)                                             \
  REFOLD_X(FutureTargetMapped)                                                 \
  REFOLD_X(ProofRootTracked)                                                   \
  REFOLD_X(MacroProofRootResolved)                                             \
  REFOLD_X(MacroProofRootIsTopLevel)                                           \
  REFOLD_X(MacroPasteWitnessPresent)                                           \
  REFOLD_X(MacroPasteWitnessWellFormed)                                        \
  REFOLD_X(MacroPasteFreeSurfaceTracked)                                       \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplayWitnessTracked)             \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplayPathUnique)                 \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplayTupleUnique)                \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplayReplayUnique)               \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplaySlicesTracked)              \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplaySlicesNonOverlapping)       \
  REFOLD_X(MacroSubtreeCertificateTracked)                                     \
  REFOLD_X(MacroCallChainWitnessTracked)                                       \
  REFOLD_X(CounterStateWitnessTracked)                                         \
  REFOLD_X(SubtreeAdmissibilityTracked)                                        \
  REFOLD_X(WholeCoverBoundsTracked)                                            \
  REFOLD_X(WholeCoverContainmentTracked)                                       \
  REFOLD_X(WholeCoverBoundaryAccountingTracked)                                \
  REFOLD_X(IncludePendingMaterializationClassified)                            \
  REFOLD_X(IncludePatchShapeTracked)                                           \
  REFOLD_X(IncludeAnchorWitnessTracked)                                        \
  REFOLD_X(IncludeAnchorByteTracked)                                           \
  REFOLD_X(IncludeConditionalOwnershipTracked)                                 \
  REFOLD_X(IncludeMappedHeaderRangeTracked)                                    \
  REFOLD_X(IncludeMappedHeaderByteRangeTracked)                                \
  REFOLD_X(IncludeSelectedConditionalBoundaryWitnessTracked)                   \
  REFOLD_X(IncludeChildBoundaryWitnessTracked)                                 \
  REFOLD_X(IncludeRightNeighborWitnessTracked)                                 \
  REFOLD_X(IncludeLeftNeighborWitnessTracked)                                  \
  REFOLD_X(IncludeDeclBoundaryWitnessTracked)                                  \
  REFOLD_X(TUAnchorPathClassified)                                             \
  REFOLD_X(TUAnchorWitnessTracked)                                             \
  REFOLD_X(TUAnchorPPGapTracked)                                               \
  REFOLD_X(TUAnchorByteTracked)                                                \
  REFOLD_X(TUExactSlotWitnessTracked)                                          \
  REFOLD_X(TUProvableEvidenceTracked)                                          \
  REFOLD_X(TUOutsideIncludeCoverageTracked)                                    \
  REFOLD_X(TUOwnerDepthStableTracked)                                          \
  REFOLD_X(ExplicitOutOfDomainResultTracked)                                   \
  REFOLD_X(PrimaryProofClassDeclared)                                          \
  REFOLD_X(OwnerRealizationWitnessTracked)

enum class ProofObligationKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(ProofObligationKind obligation) {
  switch (obligation) {
#define REFOLD_X(name)                                                         \
  case ProofObligationKind::name:                                              \
    return #name;
    REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_PROOF_OBLIGATION_KIND_LIST

/// \brief Why a local proof contract could not be discharged.
#define REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X)                             \
  REFOLD_X(None)                                                               \
  REFOLD_X(PendingMaterialization)                                             \
  REFOLD_X(MissingAcceptedPathClassification)                                  \
  REFOLD_X(MissingFutureTargetMapping)                                         \
  REFOLD_X(MissingProofRoot)                                                   \
  REFOLD_X(MissingMacroProofRootResolution)                                    \
  REFOLD_X(NonTopLevelMacroProofRoot)                                          \
  REFOLD_X(MissingPasteWitness)                                                \
  REFOLD_X(MalformedPasteWitness)                                              \
  REFOLD_X(UnexpectedPasteSurface)                                             \
  REFOLD_X(MissingRecursiveTupleGeneratedCalleeReplayWitness)                  \
  REFOLD_X(NonUniqueRecursiveTupleGeneratedCalleeReplayPath)                   \
  REFOLD_X(NonUniqueRecursiveTupleGeneratedCalleeReplayTuple)                  \
  REFOLD_X(NonUniqueRecursiveTupleGeneratedCalleeReplaySolution)               \
  REFOLD_X(MissingRecursiveTupleGeneratedCalleeReplaySlices)                   \
  REFOLD_X(OverlappingRecursiveTupleGeneratedCalleeReplaySlices)               \
  REFOLD_X(MissingSubtreeCertificate)                                          \
  REFOLD_X(MissingCallChainWitness)                                            \
  REFOLD_X(MissingCounterStateWitness)                                         \
  REFOLD_X(MissingSubtreeAdmissibility)                                        \
  REFOLD_X(MissingWholeCoverBounds)                                            \
  REFOLD_X(MissingWholeCoverContainment)                                       \
  REFOLD_X(MissingWholeCoverBoundaryAccounting)                                \
  REFOLD_X(MissingIncludePatchShape)                                           \
  REFOLD_X(MissingIncludeAnchorWitness)                                        \
  REFOLD_X(MissingIncludeAnchorByte)                                           \
  REFOLD_X(MissingConditionalOwnership)                                        \
  REFOLD_X(MissingMappedHeaderRange)                                           \
  REFOLD_X(MissingMappedHeaderByteRange)                                       \
  REFOLD_X(MissingIncludeSelectedConditionalBoundaryWitness)                   \
  REFOLD_X(MissingIncludeChildBoundaryWitness)                                 \
  REFOLD_X(MissingIncludeRightNeighborWitness)                                 \
  REFOLD_X(MissingIncludeLeftNeighborWitness)                                  \
  REFOLD_X(MissingIncludeDeclBoundaryWitness)                                  \
  REFOLD_X(MissingTUAnchorClassification)                                      \
  REFOLD_X(MissingTUAnchorWitness)                                             \
  REFOLD_X(MissingTUAnchorGap)                                                 \
  REFOLD_X(MissingTUAnchorByte)                                                \
  REFOLD_X(MissingTUExactSlotWitness)                                          \
  REFOLD_X(MissingTUProvableAnchorWitness)                                     \
  REFOLD_X(MissingTUOutsideIncludeCoverageProof)                               \
  REFOLD_X(MissingTUOwnerDepthStability)                                       \
  REFOLD_X(ExplicitOutOfDomainResult)                                          \
  REFOLD_X(MissingPrimaryProofClass)                                           \
  REFOLD_X(MissingOwnerRealizationWitness)

enum class ProofFailureReason : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(ProofFailureReason reason) {
  switch (reason) {
#define REFOLD_X(name)                                                         \
  case ProofFailureReason::name:                                               \
    return #name;
    REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "None";
}
#undef REFOLD_PROOF_FAILURE_REASON_LIST

/// \brief Compact record of class-local obligation discharge.
struct ProofDischargeRecord {
  ProofDischargeStatus status = ProofDischargeStatus::Unknown;
  ProofFailureReason failureReason = ProofFailureReason::None;
  ProofObligationKind failedObligation = ProofObligationKind::Unknown;
  uint16_t obligationsEvaluated = 0;
  uint16_t obligationsSatisfied = 0;
};

/// \brief Accumulates deterministic local proof-obligation discharge state.
///
/// Callers use Require() for ordinary obligations, Fail() for an explicit
/// fail-closed rejection, and Finish() to normalize an otherwise untouched
/// accumulator to a discharged record.  The first failure remains the canonical
/// diagnostic reason so later checks cannot overwrite the original proof gap.
/// The accumulator lives alongside ProofDischargeRecord because its only
/// dependency is that record and the obligation/reason enums above; promoting
/// it to RefoldProofVocabulary.h would create a header cycle.
struct ProofDischargeAccumulator {
  ProofDischargeRecord record;

  explicit ProofDischargeAccumulator(
      ProofDischargeStatus initialStatus = ProofDischargeStatus::Unknown) {
    record.status = initialStatus;
  }

  /// Record a satisfied obligation.  The kind names the obligation for the
  /// caller's own readability; the accumulator keeps only the counts.
  void Satisfy(ProofObligationKind /*obligation*/) {
    ++record.obligationsEvaluated;
    ++record.obligationsSatisfied;
    if (record.status == ProofDischargeStatus::Unknown)
      record.status = ProofDischargeStatus::Discharged;
  }

  /// Record a failed obligation without overwriting the first failure reason.
  void Fail(ProofObligationKind obligation, ProofFailureReason reason) {
    ++record.obligationsEvaluated;
    if (record.failedObligation == ProofObligationKind::Unknown)
      record.failedObligation = obligation;
    if (record.failureReason == ProofFailureReason::None)
      record.failureReason = reason;
    record.status = ProofDischargeStatus::Rejected;
  }

  /// Check one obligation and update the discharge record monotonically.
  void Require(bool condition, ProofObligationKind obligation,
               ProofFailureReason reason) {
    if (condition)
      Satisfy(obligation);
    else
      Fail(obligation, reason);
  }

  /// Return the normalized record, discharging an untouched accumulator.
  ProofDischargeRecord Finish() {
    if (record.status == ProofDischargeStatus::Unknown)
      record.status = ProofDischargeStatus::Discharged;
    return record;
  }
};
struct ProofSummary;
/// \brief Canonical theorem-facing proof carried by an emitted result.
///
/// This is the single object that answers why a selected artifact is
/// theorem-admissible.  Construction provenance such as AcceptedPathKind and
/// AcceptedProofClass may still exist below this layer, but they are not
/// theorem authority: they must first normalize into exactly one
/// `theoremClass` plus the typed witnesses copied here.  The carrier is
/// intentionally value-only and side-effect-free so ProofSummary can own it
/// without changing selector or emission semantics.
struct EmittedProof {
  TheoremProofClass theoremClass = TheoremProofClass::Unknown;
  ProofDischargeRecord discharge;

  std::optional<OwnerRealizationWitness> ownerRealization;
  std::optional<StructuralHunkTilingWitness> mixedOwnerTiling;
  std::optional<TUAnchorWitness> tuAnchor;
  std::optional<IncludeAnchorWitness> includeAnchor;
  std::optional<SuffixStabilityWitness> suffixStability;
  std::optional<TerminalFallbackWitness> terminalFallback;

  bool HasFinalTheoremClass() const {
    return theoremClass != TheoremProofClass::Unknown;
  }
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFDISCHARGETYPES_H

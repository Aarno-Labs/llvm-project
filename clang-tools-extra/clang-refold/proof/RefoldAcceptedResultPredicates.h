//===--- RefoldAcceptedResultPredicates.h ---------------------*- C++ -*-===//
//
// Small stateless predicates over `AcceptedResultCandidate` used by
// several proof services.
//
// Kept as free functions in this dedicated leaf header (rather than as
// static methods on `RefoldProofLattice`) so consumers like the
// accepted-result ranker, witness equivalence-key builder, and
// terminal-fallback sink can depend on them without pulling in the
// full lattice façade.
//
// Every entity here is a pure function of the candidate itself; none
// touches lattice state, and all are defined inline to stay
// zero-overhead while keeping this header a leaf.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTPREDICATES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTPREDICATES_H

#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldProofDischargeTypes.h"
#include "proof/RefoldTheoremTypes.h"

#include <utility>

namespace clang {
namespace refold {

/// Return true when \p candidate failed only the nested-macro
/// top-level selector rule.  Selection logic uses this to distinguish
/// a selector-only nested proof from a fully rejected macro candidate.
inline bool AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
    const AcceptedResultCandidate &candidate) {
  if (candidate.kind != AcceptedResultCandidateKind::MacroPatch)
    return false;

  const ProofDischargeRecord &discharge = candidate.proofSummary.discharge;
  return discharge.status == ProofDischargeStatus::Rejected &&
         discharge.failedObligation ==
             ProofObligationKind::MacroProofRootIsTopLevel &&
         discharge.failureReason ==
             ProofFailureReason::NonTopLevelMacroProofRoot;
}

/// Return the primary `EmissionPathKind` implied by an
/// `AcceptedResultCandidateKind`.  Used by the emission-path
/// inventory and by terminal-fallback classification.
inline EmissionPathKind
PrimaryEmissionPathForCandidateKind(AcceptedResultCandidateKind kind) {
  switch (kind) {
  case AcceptedResultCandidateKind::MacroPatch:
    return EmissionPathKind::MacroPatch;
  case AcceptedResultCandidateKind::IncludePatch:
    return EmissionPathKind::IncludePatch;
  case AcceptedResultCandidateKind::TUAnchor:
    return EmissionPathKind::TUAnchor;
  case AcceptedResultCandidateKind::TUTextEdit:
    return EmissionPathKind::TUTextEdit;
  case AcceptedResultCandidateKind::TerminalOutOfDomain:
    return EmissionPathKind::TerminalOutOfDomain;
  case AcceptedResultCandidateKind::Unknown:
    return EmissionPathKind::Unknown;
  }
  return EmissionPathKind::Unknown;
}

/// Rebuild the emission-path inventory for one candidate.
///
/// Deterministic and side-effect free except for the candidate's inventory
/// field.  Every accepted-carrier builder calls it after it finishes mutating
/// the proof summary, so an overlay path cannot go stale when a witness is
/// attached late — mixed-owner tiling after owner realization, for example.
///
/// Mixed-owner tiling and owner realization are theorem/proof overlays that a
/// macro, include, or TU primary emitted surface can carry.  Recording them
/// explicitly lets the proof model force those surviving paths through the
/// accepted-result gate without treating them as separate primary surfaces.
inline void RefreshAcceptedCandidateEmissionPathInventory(
    AcceptedResultCandidate &candidate) {
  EmissionPathInventory inventory;
  inventory.Add(PrimaryEmissionPathForCandidateKind(candidate.kind));

  if (candidate.proofSummary.hasMixedOwnerTilingWitness ||
      candidate.proofSummary.theoremClass ==
          TheoremProofClass::MixedOwnerTilingProof) {
    inventory.Add(EmissionPathKind::MixedOwnerTilingSegment);
  }

  if (candidate.proofSummary.hasOwnerRealizationWitness ||
      candidate.proofSummary.theoremClass ==
          TheoremProofClass::OwnerRealizationProof) {
    inventory.Add(EmissionPathKind::OwnerRealizationMaterialization);
  }

  candidate.emissionPaths = std::move(inventory);
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTPREDICATES_H

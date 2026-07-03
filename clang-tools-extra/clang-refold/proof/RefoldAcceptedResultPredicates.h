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
// Both predicates are pure functions of the candidate's proof-discharge
// record; they touch no lattice state and are defined inline here to
// stay zero-overhead while keeping this header a leaf.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTPREDICATES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTPREDICATES_H

#include "proof/RefoldAcceptedResultTypes.h"

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

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTPREDICATES_H

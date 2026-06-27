//===--- RefoldProofDischarge.h ------------------------------*- C++ -*-===//
//
// Shared proof-obligation discharge bookkeeping for clang-refold.
//
// This header owns the small deterministic accumulator used by proof builders
// while they check local theorem obligations.  The accumulator records only the
// first failed obligation/reason pair and monotonically counts evaluated and
// satisfied obligations; it does not rank candidates or choose fallback paths.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFDISCHARGE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFDISCHARGE_H

#include "proof/RefoldAcceptedResultTypes.h"

namespace clang {
namespace refold {

/// Accumulates deterministic local proof-obligation discharge state.
///
/// Callers use Require() for ordinary obligations, Fail() for an explicit
/// fail-closed rejection, and Finish() to normalize an otherwise untouched
/// accumulator to a discharged record.  The first failure remains the canonical
/// diagnostic reason so later checks cannot overwrite the original proof gap.
struct ProofDischargeAccumulator {
  ProofDischargeRecord record;

  explicit ProofDischargeAccumulator(
      ProofDischargeStatus initialStatus = ProofDischargeStatus::Unknown) {
    record.status = initialStatus;
  }

  /// Record a satisfied obligation.
  void Satisfy(ProofObligationKind obligation) {
    (void)obligation;
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

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFDISCHARGE_H

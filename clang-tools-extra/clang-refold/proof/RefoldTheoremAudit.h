//===--- RefoldTheoremAudit.h ----------------------------------*- C++ -*-===//
//
// Central theorem/audit ledger and legacy-authority audit service.
//
// RefoldTheoremAudit owns the policy for theorem-audit counters, no-legacy
// findings, terminal-fallback audit normalization, and emission-boundary
// rejection.  Extracted planning/proof services should depend on this service
// instead of carrying one-off audit callback bundles that forward back into
// RefoldEngine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMAUDIT_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMAUDIT_H

#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofTypes.h"
#include "proof/RefoldTerminalProof.h"
#include "proof/RefoldWitnessTypes.h"

#include "llvm/ADT/StringRef.h"

#include <functional>
#include <optional>
#include <string>

namespace clang {
namespace refold {

class RefoldTerminalProofSink;

/// Centralized theorem-audit service.
///
/// The service owns audit policy and the mutable theorem-audit ledger.  It does
/// not own proof ranking or terminal-witness construction; those remain in the
/// proof lattice and are exposed through narrow query hooks to avoid a service
/// construction cycle between RefoldProofLattice and the audit ledger.
class RefoldTheoremAudit {
public:
  /// Exceptional callback set for theorem queries that are still owned by the
  /// proof lattice.  RefoldTheoremAudit is constructed before the lattice and
  /// the lattice calls back into the audit service, so these remain late-bound
  /// query hooks until that construction cycle is split.
  struct Hooks {
    /// Return the normalized final theorem class for an accepted candidate.
    std::function<std::optional<TheoremProofClass>(
        const AcceptedResultCandidate &)>
        normalizeAcceptedProof;

    /// Rebuild the expected normalized proof summary for a macro patch.
    std::function<ProofSummary(const MacroPatch &)> classifyMacroPatchProof;

    /// Return the current witness-resolver mode used by invariant enforcement.
    std::function<WitnessResolverMode()> getWitnessResolverMode;

    /// Build terminal fallback proof data from the current terminal sink ledger.
    std::function<TerminalFallbackWitness()> buildTerminalFallbackWitness;

    /// Normalize a terminal witness into the shared accepted-candidate carrier.
    std::function<AcceptedResultCandidate(const TerminalFallbackWitness &)>
        buildAcceptedTerminalCandidate;
  };

  RefoldTheoremAudit(TheoremAuditStats &audit,
                     const RefoldTerminalProofSink &terminalSink, bool strict,
                     Hooks hooks);

  /// Return whether no-legacy findings are theorem-enforced for this run.
  bool IsNoLegacyAuditEnabled() const;

  /// Build a deterministic no-legacy audit record.
  static LegacyAuditEvidence
  MakeLegacyAuditEvidence(LegacyPathKind kind, llvm::StringRef role,
                          llvm::StringRef detail = llvm::StringRef());

  /// Reset the per-run theorem audit ledger.
  void Reset() const { audit_ = TheoremAuditStats{}; }

  /// Expose the mutable audit counters for services that still accumulate
  /// low-level proof statistics directly while their own ledgers are extracted.
  TheoremAuditStats &Stats() const { return audit_; }

  /// Record the first theorem-audit violation encountered in this run.
  void NoteTheoremAuditViolation(llvm::StringRef detail) const;

  /// Emit one no-legacy finding and attach it to the theorem audit.
  void ReportNoLegacyAuditFinding(const LegacyAuditEvidence &evidence) const;

  /// Enforce the final strict theorem-audit invariant.
  void EnforceTheoremAuditInvariants() const;

  /// Record and enforce witness-resolver theorem-domain completion criteria.
  void RecordWitnessResolverTheoremAudit(
      const WitnessResolverDecision &decision) const;

  bool AuditProofSummaryForLegacyAuthority(const ProofSummary &summary,
                                           llvm::StringRef role) const;
  bool AuditAcceptedResultCandidateForLegacyAuthority(
      const AcceptedResultCandidate &candidate, llvm::StringRef role) const;
  bool AuditMacroPatchProofForLegacyAuthority(const MacroPatch &patch,
                                              llvm::StringRef role) const;
  bool AuditTerminalFallbackForLegacyAuthority(
      const TerminalFallbackProofFailure &failure, llvm::StringRef role) const;
  bool AuditExpansionFallbackBranchClassification(
      const ExpansionFallbackBranchClassification &classification,
      llvm::StringRef role) const;
  bool AuditExpansionFallbackAcceptedCandidate(
      const ExpansionFallbackBranchClassification &classification,
      const AcceptedResultCandidate &candidate, llvm::StringRef role) const;
  bool AuditTerminalFallbackProofFailure(
      const TerminalFallbackProofFailure &failure, llvm::StringRef role) const;

  /// Record that terminal fallback reached emission without any structured
  /// proof-failure rows to audit.
  void RecordTerminalFallbackProofFailureListMissing(
      llvm::StringRef detail) const;

  /// Report a no-legacy emission-boundary finding and fail closed in strict mode.
  bool RejectNoLegacyAuditFindingIfStrict(
      const LegacyAuditEvidence &evidence,
      const TerminalFallbackProofFailure &failure) const;

  TerminalFallbackProofFailure
  MakeMissingSelectedMacroPatchCarrierFailure() const;

  bool RejectMissingSelectedMacroPatchCarrier(const MacroPatch &patch,
                                              llvm::StringRef role,
                                              llvm::StringRef detail) const;

  /// Validate state-transition gateway audit counters before bytes are emitted.
  bool AuditStateTransitionGatewayProofs(llvm::StringRef emissionStage,
                                         llvm::StringRef emissionOwner) const;

  /// Normalize terminal fallback ledger state onto the theorem-audit carrier.
  void RecordTerminalFallbackTheoremAudit() const;

  /// Build the compact strict-mode invariant diagnostic string.
  std::string BuildTheoremAuditInvariantDetail() const;

  void RecordDirectStateCheckClosure(
      DirectStateCheckKind checkKind, OwnerStateComponent component,
      DirectStateCheckClosureKind closure, StateMutationKind mutation,
      llvm::StringRef stage, llvm::StringRef detail) const;
  void RecordOwnerStateGraphAudit(const OwnerStateGraphAuditStats &audit) const;
  void RecordGraphIncomparableNode() const;
  void RecordStateTransitionGatewayCheck() const;
  void RecordStateTransitionGatewayStable() const;
  void RecordStateTransitionGatewayTerminalFailure() const;
  void RecordStateTransitionUnknownComponentViolation() const;
  void RecordStateTransitionUnknownMutationViolation() const;
  void RecordStateTransitionNoneWitnessViolation() const;
  void RecordStateTransitionGatewayViolation(llvm::StringRef detail) const;

private:
  TheoremAuditStats &audit_;
  const RefoldTerminalProofSink &terminalSink_;
  bool strict_ = false;
  Hooks hooks_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMAUDIT_H

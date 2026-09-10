//===--- RefoldTheoremAudit.h ----------------------------------*- C++ -*-===//
//
// Central theorem/audit ledger and legacy-authority audit service.
//
// The service owns audit policy and the mutable theorem-audit ledger.  It
// does not own proof ranking or terminal-witness construction; those remain
// with their producing services and are exposed through narrow query hooks
// to avoid a service-construction cycle between RefoldProofLattice and the
// audit ledger.  The per-attempt stats reporting helpers live here because
// they read the same ledger and are only invoked at end-of-attempt.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMAUDIT_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMAUDIT_H

#include "core/RefoldLog.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldAcceptancePathTypes.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTerminalProofSink.h"

#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>

namespace clang {
namespace refold {

class RefoldProofLattice;

class RefoldModel;

/// Centralized theorem-audit service.
class RefoldTheoremAudit {
public:
  RefoldTheoremAudit(TheoremAuditStats &audit,
                     const RefoldTerminalProofSink &terminalSink, bool strict,
                     const bool &alignmentSemanticTheoremActive);

  /// Bind the proof lattice this audit queries, completing the service graph.
  ///
  /// The audit is constructed before the lattice because the lattice takes the
  /// audit by reference; the reverse edge is therefore bound in a second phase,
  /// once both services exist.  Splitting the cycle this way is what lets the
  /// five lattice queries below be direct calls rather than a late-bound
  /// callback bundle.  Every query site tolerates an unbound lattice, so an
  /// audit that is never bound degrades to "no lattice-derived facts" instead
  /// of dereferencing null.
  void BindProofLattice(const RefoldProofLattice &lattice);

  /// Return whether semantic no-legacy auditing is active for this run.
  ///
  /// The audit is tied directly to strict/theorem validation: when this returns
  /// true, findings are theorem state and may force the same fail-closed
  /// terminal result as other undischarged emission-boundary obligations. There
  /// are no separate environment-variable controls for this theorem invariant.
  bool IsNoLegacyAuditEnabled() const;

  /// Build a deterministic no-legacy audit finding.
  static LegacyAuditEvidence
  MakeLegacyAuditEvidence(LegacyPathKind kind, llvm::StringRef role,
                          llvm::StringRef detail = llvm::StringRef());

  /// Reset the per-run theorem audit ledger.
  void Reset() const { audit_ = TheoremAuditStats{}; }

  /// Expose the mutable audit counters for services that accumulate low-level
  /// proof statistics directly before the final theorem-audit invariant is
  /// enforced.
  TheoremAuditStats &Stats() const { return audit_; }

  /// Record the first theorem-audit violation encountered in this run.
  void NoteTheoremAuditViolation(llvm::StringRef detail) const;

  /// Emit one no-legacy finding and attach it to the theorem audit.
  ///
  /// No-legacy cleanliness is part of the strict/theorem invariant: a strict
  /// refold run may not merely print findings and still report a satisfied
  /// theorem audit.  Keeping this as the single reporting entry point avoids
  /// duplicated counter/violation policy at individual audit sites.
  void ReportNoLegacyAuditFinding(const LegacyAuditEvidence &evidence) const;

  /// Enforce the final strict theorem-audit invariant.
  ///
  /// The theorem audit is an invariant checker rather than only a descriptive
  /// dashboard. This helper normalizes any surviving counter-based violations
  /// onto the theorem state and, in strict mode, requests the one explicit
  /// terminal fallback instead of allowing a structurally-refolded result to
  /// escape with a violated theorem audit.
  void EnforceTheoremAuditInvariants() const;

  /// Record and enforce the final strict-domain resolver completion criteria.
  void RecordWitnessResolverTheoremAudit(
      const WitnessResolverDecision &decision) const;

  /// Audit a normalized proof summary for legacy-authority dependencies.
  ///
  /// The summary is acceptable only when its construction path, theorem class,
  /// local discharge, emitted proof, and typed witnesses agree without relying
  /// on a path-specific legacy mirror or an unclassified fallback branch.
  bool AuditProofSummaryForLegacyAuthority(const ProofSummary &summary,
                                           llvm::StringRef role) const;
  /// Audit an accepted-result candidate for legacy-authority dependencies.
  ///
  /// This is the candidate-level theorem boundary: the candidate must normalize
  /// through its proof summary instead of deriving authority from raw artifact
  /// kind, construction order, or path-local fallback bits.
  bool AuditAcceptedResultCandidateForLegacyAuthority(
      const AcceptedResultCandidate &candidate, llvm::StringRef role) const;
  /// Audit the selected macro-patch proof carrier for legacy-authority
  /// dependencies before the patch can act as an emitted artifact.
  bool AuditMacroPatchProofForLegacyAuthority(const MacroPatch &patch,
                                              llvm::StringRef role) const;
  /// Audit one terminal fallback proof failure for legacy-authority
  /// dependencies.  Terminal fallback is allowed only as an explicit, named,
  /// structured theorem boundary.
  bool AuditTerminalFallbackForLegacyAuthority(
      const TerminalFallbackProofFailure &failure, llvm::StringRef role) const;
  /// Audit an expansion-fallback branch classification before it is accepted
  /// as theorem state.  Unclassified fallback behavior must not survive as a
  /// diagnostic-only branch.
  bool AuditExpansionFallbackBranchClassification(
      const ExpansionFallbackBranchClassification &classification,
      llvm::StringRef role) const;
  /// Audit an expansion-fallback accepted candidate and its branch
  /// classification together so the carrier cannot satisfy the proof lattice
  /// while the branch itself remains a legacy fallback decision.
  bool AuditExpansionFallbackAcceptedCandidate(
      const ExpansionFallbackBranchClassification &classification,
      const AcceptedResultCandidate &candidate, llvm::StringRef role) const;
  /// Validate that one terminal fallback proof failure is a named, structured
  /// failed obligation rather than an opaque raw-B escape.
  ///
  /// This runs immediately before raw-B emission.  A generic
  /// MissingProducerFacts reason must identify the missing fact through the
  /// structured context carrier; implementation-specific reasons such as an
  /// unmappable include envelope are already self-identifying.
  bool
  AuditTerminalFallbackProofFailure(const TerminalFallbackProofFailure &failure,
                                    llvm::StringRef role) const;

  /// Record that terminal fallback reached emission without any structured
  /// proof-failure rows to audit.
  void
  RecordTerminalFallbackProofFailureListMissing(llvm::StringRef detail) const;

  /// Report a no-legacy emission-boundary finding and fail closed in strict
  /// mode.
  bool RejectNoLegacyAuditFindingIfStrict(
      const LegacyAuditEvidence &evidence,
      const TerminalFallbackProofFailure &failure) const;

  /// Build the canonical terminal failure used when a MacroPatch reaches an
  /// emission boundary without the selected accepted-result carrier required
  /// for every emitted MacroPatch.
  TerminalFallbackProofFailure
  MakeMissingSelectedMacroPatchCarrierFailure() const;

  /// Enforce the emission-boundary invariant for a MacroPatch whose selected
  /// carrier is missing.  Strict engine runs fail closed immediately without
  /// manufacturing replacement proof authority from the raw MacroPatch.
  bool RejectMissingSelectedMacroPatchCarrier(const MacroPatch &patch,
                                              llvm::StringRef role,
                                              llvm::StringRef detail) const;

  /// Validate state-transition gateway audit counters before bytes are
  /// emitted.
  bool AuditStateTransitionGatewayProofs(llvm::StringRef emissionStage,
                                         llvm::StringRef emissionOwner) const;

  /// Normalize terminal fallback ledger state onto the theorem-audit
  /// carrier.
  void RecordTerminalFallbackTheoremAudit() const;

  /// Build the compact strict-mode invariant diagnostic string.
  std::string BuildTheoremAuditInvariantDetail() const;

  /// Record a direct state check closure into the theorem-audit ledger.
  ///
  /// Direct state checks are allowed only when they are routed through the
  /// state-transition gateway model; this record preserves the checked
  /// component, closure kind, mutation kind, and diagnostic stage for the final
  /// strict invariant.
  void RecordDirectStateCheckClosure(DirectStateCheckKind checkKind,
                                     OwnerStateComponent component,
                                     DirectStateCheckClosureKind closure,
                                     StateMutationKind mutation,
                                     llvm::StringRef stage,
                                     llvm::StringRef detail) const;
  /// Merge owner-state graph audit counters into the theorem-audit ledger.
  void RecordOwnerStateGraphAudit(const OwnerStateGraphAuditStats &audit) const;
  /// Record that owner-state graph comparison encountered incomparable nodes.
  void RecordGraphIncomparableNode() const;
  /// Record that a state-transition gateway check was performed.
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
  const bool &alignmentSemanticTheoremActive_;
  const RefoldProofLattice *proofLattice_ = nullptr;
};

//===----------------------------------------------------------------------===//
// Per-attempt stats reporting helpers.
//===----------------------------------------------------------------------===//

/// Reset \p stats for a new refold attempt and seed the immutable totals
/// from \p model: total recorded includes and the count of top-level macro
/// invocation roots (children with a `callerMacroId` are excluded).
void resetRefoldAttemptStats(RefoldStats &stats, const RefoldModel &model);

/// Emit a one-line `info`-level summary of the final refold attempt.
///
/// Reports how many includes and top-level macro invocations remained
/// expanded relative to the model's totals.  \p hasTerminalRequest annotates
/// the summary when the attempt fell back to the explicit terminal raw-B
/// carrier.  \p passRole names which of one run's nested passes produced these
/// numbers -- a production attempt, or one realized candidate alignment map --
/// because every such pass emits this line and they are otherwise identical.
void emitRefoldAttemptStatsSummary(const RefoldStats &stats,
                                   bool hasTerminalRequest,
                                   llvm::StringRef passRole);

/// Emit a readable theorem-audit summary for the current attempt.
///
/// The first line answers the operational question: did the emitted result
/// satisfy the strict theorem audit?  Follow-up debug lines group the dense
/// counters by proof obligation so a failure can be diagnosed without
/// decoding one very long ledger row.  \p passRole attributes the line to one
/// of the run's nested passes, as for `emitRefoldAttemptStatsSummary()`.
void emitTheoremAuditSummary(const TheoremAuditStats &audit,
                             llvm::StringRef passRole);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMAUDIT_H

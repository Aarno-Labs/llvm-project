//===--- RefoldWitnessTrace.h ----------------------------------*- C++ -*-===//
//
// Witness trace and proof-audit logging service for clang-refold.
//
// Owns the proof-trace output that is gated by the configured witness resolver
// mode.  The service is read-only: it formats and emits trace lines but does
// not maintain any per-attempt state of its own.  The resolver mode is derived
// from the same strict + ProofAuditMode inputs the lattice already consumes, so
// the lattice and the trace service report the same gating decision.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSTRACE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSTRACE_H

#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremAudit.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>

namespace clang {
namespace refold {

/// Emits proof-audit trace records for witness resolution and terminal
/// fallback.  The tracer is read-only: strict-mode decisions are computed by
/// the resolver and audit services, then rendered here when proof tracing is
/// enabled.
class RefoldWitnessTrace {
public:
  RefoldWitnessTrace(bool strict, const ProofAuditMode &proofAuditMode,
                     const bool &alignmentSemanticTheoremActive);

  /// Compute the active witness resolver mode from the configured strict flag
  /// and proof-audit mode.  This is the single source of truth that gates
  /// proof-trace emission and resolver-decision recording.
  WitnessResolverMode GetWitnessResolverMode() const;

  /// Return whether proof-trace lines should be emitted in this run.
  bool ShouldEmitProofLog() const;

  /// Deterministic FNV-1a hex digest used to keep proof-trace lines stable
  /// across runs without relying on implementation-specific hash seeds.
  static std::string FormatWitnessTraceHash(llvm::StringRef text);

  void
  TraceWitnessStrictDomain(llvm::StringRef role,
                           const WitnessStrictDomainDecision &decision) const;

  void TraceWitnessClosureLedger(const WitnessResolverDecision &decision) const;

  void
  TraceWitnessResolverDecision(const WitnessResolverDecision &decision) const;

  void TraceWitnessCompositionDecision(
      llvm::StringRef role, const WitnessCompositionDecision &decision) const;

  void TraceWitnessEmitted(const RefoldWitness &witness) const;

  void TraceWitnessRejected(const RefoldWitness &witness,
                            WitnessRejectReason reason,
                            llvm::StringRef detail) const;

  void TraceWitnessAmbiguity(llvm::StringRef role, uint64_t candidateCount,
                             uint64_t selectableCount,
                             uint64_t ambiguityClassCount) const;

  void TraceWitnessSelectionProbe(llvm::StringRef role, uint64_t candidateCount,
                                  uint64_t proofValidCount,
                                  uint64_t proofInvalidCount,
                                  uint64_t equivalenceClassCount,
                                  uint64_t completeWitnessCount,
                                  uint64_t incompleteWitnessCount) const;

  void TraceWitnessChosen(const RefoldWitness &witness,
                          uint64_t selectedIndex) const;

  /// Report a candidate triple that breaks one of the strict-order laws the
  /// representative selector depends on.  The record names the offending
  /// candidate indices so the defective comparison can be reproduced without
  /// re-running an approximate ranking.
  void TraceSelectionOrderViolation(llvm::StringRef role,
                                    const SelectionOrderViolation &violation,
                                    uint64_t selectableCount) const;

  void TraceWitnessFallback(const TerminalFallbackRequest &request) const;

private:
  bool strict_;
  const ProofAuditMode &proofAuditMode_;
  const bool &alignmentSemanticTheoremActive_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSTRACE_H

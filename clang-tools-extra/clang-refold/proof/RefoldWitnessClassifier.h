//===--- RefoldWitnessClassifier.h ------------------------------*- C++ -*-===//
//
// Witness classification service for clang-refold.
//
// Owns the pure-function vocabulary that maps accepted-path kinds, terminal
// fallback failures, resolver fallback reasons, and witness composition state
// onto the proof families, producer kinds, boundary classes, fallback classes,
// strict-domain decisions, and strict-domain obligations consumed by the
// proof lattice, resolver, and trace subsystems.
//
// Every entry point is a stateless free function in `clang::refold`.  The
// classifier owns proof-vocabulary policy independently of RefoldProofLattice,
// so callers can classify witnesses without threading a lattice instance
// through pure vocabulary checks.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSCLASSIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSCLASSIFIER_H

#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremTypes.h"

#include "llvm/ADT/StringRef.h"

namespace clang {
namespace refold {

/// Map an accepted-path kind onto the proof family that emits it.
::clang::refold::WitnessProofFamily
witnessFamilyForAcceptedPath(AcceptedPathKind path);

/// Map an accepted-path kind onto the producer kind that established it.
::clang::refold::WitnessProducerKind
witnessProducerKindForAcceptedPath(AcceptedPathKind path);

/// Map an accepted-result candidate onto its boundary class.
::clang::refold::WitnessBoundaryClass witnessBoundaryClassForAcceptedCandidate(
    const AcceptedResultCandidate &candidate);

/// Classify a terminal-fallback proof failure as a witness fallback class.
::clang::refold::WitnessFallbackClass
classifyTerminalFallbackFailure(const TerminalFallbackProofFailure &failure);

/// Classify a resolver fallback failure-reason string into a fallback class.
::clang::refold::WitnessFallbackClass
classifyResolverFallbackReason(llvm::StringRef reason,
                               const WitnessCompositionDecision &composition);

/// Return the strict-domain obligation implied by a fallback class.
::clang::refold::WitnessStrictDomainObligation
strictDomainObligationForFallbackClass(WitnessFallbackClass fallbackClass);

/// Derive the strict-domain decision for a resolver decision.
::clang::refold::WitnessStrictDomainDecision
classifyStrictDomainForResolver(const WitnessResolverDecision &decision);

/// Derive the strict-domain decision for a terminal-fallback failure.
::clang::refold::WitnessStrictDomainDecision
classifyStrictDomainForTerminalFallback(
    const TerminalFallbackProofFailure &failure);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSCLASSIFIER_H

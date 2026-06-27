//===--- RefoldTerminalProof.h ---------------------------------*- C++ -*-===//
//
// Terminal fallback proof vocabulary for clang-refold.
//
// This module owns the data-only proof objects that justify the explicit
// terminal raw-B carrier: the failed obligation, the local failure reason, the
// normalized theorem-facing failure kind, structured failure context, and the
// compact witness/request carriers derived from those facts.  The mutable
// ordered request list is owned by RefoldTerminalProofSink; the top-level
// refold orchestration coordinates the final raw-B emission path.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOF_H

#include "core/RefoldLog.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

/// Named proof obligation that forced the terminal raw-B result.
///
/// Terminal fallback is part of the proof system, not a convenience escape.
/// Every request must name the theorem obligation that failed.  Keep these
/// obligations close to the closed-domain contract: owner closure,
/// deterministic tiling, state stability, producer-fact availability, and final
/// validation are the only acceptable reasons to emit raw B.
enum class TerminalFallbackObligationKind : uint8_t {
  Unknown,
  OwnerClosedCover,
  DeterministicMixedOwnerTiling,
  StateTransitionClosure,
  PragmaBoundaryKnown,
  LineControlStateProducerProven,
  CounterStateStabilizable,
  MacroStateStabilizable,
  IncludeGuardStateStabilizable,
  ConditionalStateStabilizable,
  ReverseSolvedDirectiveForbidden,
  InvocationPreservationWellFormed,
  ProducerFactsAvailable,
  FinalValidationSucceeded,
  IncludeRealizationBEnvelopeMapped,
  EmissionArtifactDischarged,
  EmissionEditSetComposable,
  TheoremAuditInvariantSatisfied,
};

llvm::StringRef toString(TerminalFallbackObligationKind obligation);

/// Concrete reason the terminal-fallback obligation failed.
///
/// This implementation-local reason preserves precise diagnostics before
/// normalization to the final theorem vocabulary below.  The reason names the
/// local domain wall; the obligation names the theorem condition.  Keeping both
/// makes terminal fallback auditable without having to reverse-engineer the
/// implementation site that requested it.
enum class TerminalFallbackFailureReason : uint8_t {
  Unknown,
  NoOwnerClosedCover,
  NoDeterministicMixedOwnerTiling,
  AmbiguousMixedOwnerTiling,
  StateTransitionConsumedAndObserved,
  UnknownPragmaCrossesBoundary,
  LineControlStateNotProducerProven,
  CounterStateNotStabilizable,
  MacroStateNotStabilizable,
  IncludeGuardStateNotStabilizable,
  ConditionalStateNotStabilizable,
  ReverseSolvedDirectiveRequired,
  MalformedInvocationPreservation,
  MissingProducerFacts,
  NoCanonicalSuffixOrder,
  ValidationFailure,
  NoTUAnchorForUnresolvedOwner,
  UnmappableIncludeBEnvelope,
  UndischargedEmissionArtifact,
  UncomposableEmissionEditSet,
  TheoremAuditInvariantViolation,
  UnclassifiedTerminalFallback,
};

llvm::StringRef toString(TerminalFallbackFailureReason reason);

/// Final theorem-facing terminal failure vocabulary.
///
/// Older/local failure reasons may remain for diagnostics, but every live
/// terminal fallback must normalize to one of these theorem failure kinds.  This
/// is the canonical answer to: "which strict-domain obligation failed strongly
/// enough to justify the terminal raw-B carrier?"
enum class TheoremFallbackFailureKind : uint8_t {
  Unknown,
  NoOwnerClosedCover,
  NoDeterministicMixedOwnerTiling,
  AmbiguousMixedOwnerTiling,
  StateTransitionConsumedAndObserved,
  UnknownPragmaCrossesBoundary,
  LineControlStateNotProducerProven,
  CounterStateNotStabilizable,
  MacroStateNotStabilizable,
  IncludeGuardStateNotStabilizable,
  ConditionalStateNotStabilizable,
  ReverseSolvedDirectiveRequired,
  MalformedInvocationPreservation,
  MissingProducerFacts,
  ValidationFailure,
};

llvm::StringRef toString(TheoremFallbackFailureKind kind);

/// Normalize an implementation-local terminal reason to the frozen theorem
/// failure vocabulary.  This is the gate that lets legacy/local diagnostics
/// coexist with a small final theorem language.
std::optional<TheoremFallbackFailureKind>
NormalizeTerminalFallbackFailureReason(TerminalFallbackFailureReason reason);

/// Structured local facts attached to a terminal proof failure.
///
/// Terminal fallback context is data, not prose.  Most fallback sites cannot
/// populate every field yet, but the carrier is intentionally stable: later
/// proof passes can attach owner, hunk, state-component, source-span,
/// token-envelope, and first-observer evidence without changing the terminal
/// witness format again.  Logs are derived from this structure; callers should
/// not encode theorem facts only inside a free-form detail string.
struct TerminalFallbackFailureContext {
  std::optional<std::string> owner;
  std::optional<uint64_t> hunk;
  std::optional<std::string> stateComponent;

  std::optional<std::string> sourcePath;
  std::optional<uint64_t> sourceBegin;
  std::optional<uint64_t> sourceEnd;

  std::optional<uint64_t> aTokenBegin;
  std::optional<uint64_t> aTokenEnd;
  std::optional<uint64_t> bTokenBegin;
  std::optional<uint64_t> bTokenEnd;

  static TerminalFallbackFailureContext ForStateComponent(llvm::StringRef name);
  static TerminalFallbackFailureContext ForHunkTokenEnvelope(
      uint64_t hunkIndex, uint64_t aBegin, uint64_t aEnd, uint64_t bBegin,
      uint64_t bEnd);

  bool Empty() const;
  std::string ToString() const;
};

/// Normalized proof failure attached to terminal fallback.
struct TerminalFallbackProofFailure {
  TerminalFallbackObligationKind obligation =
      TerminalFallbackObligationKind::Unknown;
  TerminalFallbackFailureReason reason = TerminalFallbackFailureReason::Unknown;
  TheoremFallbackFailureKind theoremFailure =
      TheoremFallbackFailureKind::Unknown;
  TerminalFallbackFailureContext context;

  std::string ToString() const;
};

/// Return whether a fallback proof failure is theorem-facing.
///
/// Terminal fallback proofs may not still say `Unknown` or `Unclassified`.  The
/// guard is deliberately small and shared by request recording, terminal-witness
/// construction, and theorem-audit reporting so a new fallback path cannot
/// quietly reintroduce an opaque raw-B escape.
bool IsClassifiedTerminalFallbackProofFailure(
    const TerminalFallbackProofFailure &failure);

/// Build a classified terminal fallback proof failure.
///
/// Use this helper at every terminal fallback request site.  It documents the
/// local proof obligation being rejected and prevents call sites from relying on
/// terminal-fallback kind alone as an implicit classification.
TerminalFallbackProofFailure MakeTerminalFallbackProofFailure(
    TerminalFallbackObligationKind obligation,
    TerminalFallbackFailureReason reason);

/// Build a classified terminal fallback proof failure with structured local
/// context.  This overload keeps context population centralized so call sites do
/// not have to duplicate the obligation/reason initialization sequence before
/// attaching owner, hunk, state, or span facts.
TerminalFallbackProofFailure MakeTerminalFallbackProofFailure(
    TerminalFallbackObligationKind obligation,
    TerminalFallbackFailureReason reason,
    TerminalFallbackFailureContext context);

/// Compact witness describing why terminal raw-B emission was selected.
///
/// This is the theorem-facing carrier for the terminal raw-B exit.  It
/// deliberately keeps only the ordered structured failures.  There is no
/// parallel "primary" scalar, request counter, or branch-local boolean: the
/// first element is the primary failed obligation and the remaining elements are
/// secondary obligations discovered before terminal emission.
struct TerminalFallbackWitness {
  std::vector<TerminalFallbackProofFailure> proofFailures;

  const TerminalFallbackProofFailure *PrimaryFailure() const;
  std::string ToString() const;
};

/// Typed construction request for the terminal raw-B carrier.
///
/// Terminal fallback construction is data-first.  The proof failure below is
/// the only semantic reason that may justify selecting the terminal
/// out-of-domain result; `stage` and `detail` are diagnostic labels used to keep
/// traces actionable.  Keeping them in the same carrier prevents call sites from
/// smuggling behavior through a free-form reason string while preserving the
/// useful human explanation in debug output.
struct TerminalFallbackRequest {
  TerminalFallbackProofFailure failure;
  std::string stage;
  std::string detail;

  std::string ToString() const;
};

TerminalFallbackRequest MakeTerminalFallbackRequest(
    TerminalFallbackProofFailure failure, llvm::StringRef stage,
    llvm::StringRef detail);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOF_H

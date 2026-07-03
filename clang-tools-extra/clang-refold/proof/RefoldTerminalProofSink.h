//===--- RefoldTerminalProofSink.h -----------------------------*- C++ -*-===//
//
// Mutable per-refold-request terminal fallback sink.
//
// The sink owns request normalization and storage.  The caller wires the
// hooks below to the current proof/audit services at construction time; the
// sink itself has no back-reference to the theorem-audit ledger.  Data-only
// terminal vocabulary (obligation/reason enums, failure context, witness,
// and request carriers) lives in RefoldProofVocabulary.h.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOFSINK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOFSINK_H

#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace clang {
namespace refold {

/// Terminal-specific callbacks used by RefoldTerminalProofSink.
///
/// The sink owns request normalization and storage.  The caller wires these
/// hooks to the current proof/audit services at construction time.  Each
/// hook is intentionally terminal-specific; this is not a generic service
/// locator.
struct RefoldTerminalProofSinkCallbacks {
  using AuditLegacyAuthorityFn = std::function<void(
      const TerminalFallbackProofFailure &, llvm::StringRef)>;
  using NoteTheoremAuditViolationFn = std::function<void(llvm::StringRef)>;
  using TraceTerminalRequestFn =
      std::function<void(const TerminalFallbackRequest &)>;

  AuditLegacyAuthorityFn auditLegacyAuthority;
  NoteTheoremAuditViolationFn noteTheoremAuditViolation;
  TraceTerminalRequestFn traceTerminalRequest;
};

/// Mutable terminal fallback request sink.
///
/// All mutating entry points are const because many proof helpers are
/// logically const and terminal fallback is a proof/audit side-channel for
/// the current refold attempt.  The sink owns the request ledger while
/// callers retain top-level control over final fallback emission.
class RefoldTerminalProofSink {
public:
  explicit RefoldTerminalProofSink(RefoldTerminalProofSinkCallbacks callbacks);

  /// Record that the current run escaped the declared proof domain.
  ///
  /// The typed request is the authoritative terminal carrier. The request's
  /// proof failure decides whether raw-B emission is justified; stage/detail
  /// strings are preserved only as diagnostics attached to that same typed
  /// request. This prevents a future fallback path from making behavior depend
  /// on an opaque string reason.
  void RequestTerminalFallback(TerminalFallbackRequest request) const;

  /// Record that the current run escaped the declared proof domain.
  ///
  /// The structured proof failure is the authoritative terminal carrier.
  /// Callers therefore pass the failed obligation directly; this routine only
  /// normalizes/audits that obligation, preserves all requests in order, and
  /// emits diagnostics derived from the proof data.  It must not derive a
  /// theorem failure from a broad aggregate fallback kind.
  void RequestTerminalFallback(TerminalFallbackProofFailure failure,
                               llvm::StringRef stage,
                               llvm::StringRef detail) const;

  /// Return whether any explicit terminal-fallback request has been recorded
  /// for the current refold attempt.
  bool HasRequest() const { return !requests_.empty(); }

  /// Return the number of ordered terminal-fallback requests preserved in the
  /// request ledger.
  std::size_t Size() const { return requests_.size(); }

  /// Return the ordered terminal-fallback request ledger.  The first classified
  /// request is the primary failed obligation used by the terminal witness.
  llvm::ArrayRef<TerminalFallbackRequest> Requests() const { return requests_; }

  /// Clear the per-attempt terminal-fallback ledger before a new pass starts.
  void Reset() const { requests_.clear(); }

private:
  RefoldTerminalProofSinkCallbacks callbacks_;
  mutable std::vector<TerminalFallbackRequest> requests_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOFSINK_H

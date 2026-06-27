//===--- RefoldTerminalProofSink.h ----------------------------*- C++ -*-===//
//
// Terminal fallback request sink for clang-refold.
//
// The terminal proof vocabulary in RefoldTerminalProof.h is data-only.  This
// service owns the mutable per-refold request ledger and the centralized policy
// for admitting a request into that ledger.  Extracted proof/planning services
// should depend on this sink instead of calling back into RefoldEngine merely to
// record that the current pass has left the strict theorem domain.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOFSINK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOFSINK_H

#include "proof/RefoldTerminalProof.h"

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
/// hooks to the current proof/audit services at construction time.  Each hook is
/// intentionally terminal-specific; this is not a generic service locator.
struct RefoldTerminalProofSinkCallbacks {
  using AuditLegacyAuthorityFn =
      std::function<void(const TerminalFallbackProofFailure &,
                         llvm::StringRef)>;
  using NoteTheoremAuditViolationFn =
      std::function<void(llvm::StringRef)>;
  using TraceTerminalRequestFn =
      std::function<void(const TerminalFallbackRequest &)>;

  AuditLegacyAuthorityFn auditLegacyAuthority;
  NoteTheoremAuditViolationFn noteTheoremAuditViolation;
  TraceTerminalRequestFn traceTerminalRequest;
};

/// Mutable terminal fallback request sink.
///
/// All mutating entry points are const because many proof helpers are logically
/// const and terminal fallback is a proof/audit side-channel for the current
/// refold attempt.  The sink owns the request ledger while callers retain
/// top-level control over final fallback emission.
class RefoldTerminalProofSink {
public:
  explicit RefoldTerminalProofSink(
      RefoldTerminalProofSinkCallbacks callbacks);


  void RequestTerminalFallback(TerminalFallbackRequest request) const;

  void RequestTerminalFallback(TerminalFallbackProofFailure failure,
                               llvm::StringRef stage,
                               llvm::StringRef detail) const;

  bool HasRequest() const { return !requests_.empty(); }

  std::size_t Size() const { return requests_.size(); }

  llvm::ArrayRef<TerminalFallbackRequest> Requests() const {
    return requests_;
  }

  void Reset() const { requests_.clear(); }

private:
  RefoldTerminalProofSinkCallbacks callbacks_;
  mutable std::vector<TerminalFallbackRequest> requests_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTERMINALPROOFSINK_H

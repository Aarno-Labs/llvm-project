//===--- RefoldTerminalProofSink.cpp --------------------------*- C++ -*-===//
//
// Centralized terminal fallback request recording for clang-refold.
//
// This file intentionally contains no edit planning and no fallback selection
// logic.  It only validates/normalizes typed terminal proof failures, records
// the ordered request ledger, and emits terminal-specific audit/trace callbacks
// supplied by the current engine object graph.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldLog.h"
#include "proof/RefoldTerminalProofSink.h"
#include "util/StringUtils.h"

#include "llvm/Support/FormatVariadic.h"

#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldTerminalProofSink::RefoldTerminalProofSink(
    RefoldTerminalProofSinkCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}


void RefoldTerminalProofSink::RequestTerminalFallback(
    TerminalFallbackProofFailure failure, StringRef stage,
    StringRef detail) const {
  RequestTerminalFallback(
      MakeTerminalFallbackRequest(std::move(failure), stage, detail));
}

void RefoldTerminalProofSink::RequestTerminalFallback(
    TerminalFallbackRequest request) const {
  TerminalFallbackProofFailure &failure = request.failure;
  const StringRef stage(request.stage);
  const StringRef detail(request.detail);

  callbacks_.auditLegacyAuthority(failure, stage);

  if (!IsClassifiedTerminalFallbackProofFailure(failure)) {
    // An unclassified fallback request would be a theorem-audit bug: it would
    // allow raw B without saying which strict-domain obligation failed.  Keep
    // the sink fail-closed by converting the request itself into a classified
    // theorem-audit violation before it enters the ordered request ledger.
    failure = MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied,
        TerminalFallbackFailureReason::TheoremAuditInvariantViolation,
        TerminalFallbackFailureContext::ForStateComponent(
            "terminalFallbackRequest"));
    callbacks_.noteTheoremAuditViolation(
        formatv("terminal fallback request lacked a classified proof "
                "failure: stage={0} detail={1}",
                stage, stringutils::showWsWithClip(detail, 200))
            .str());
  }

  if (failure.reason == TerminalFallbackFailureReason::MissingProducerFacts &&
      failure.context.Empty()) {
    // Generic MissingProducerFacts requests must identify the missing fact in
    // structured context.  The diagnostic detail is kept for humans, but the
    // theorem-facing carrier is converted to an explicit audit violation.
    failure = MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied,
        TerminalFallbackFailureReason::TheoremAuditInvariantViolation,
        TerminalFallbackFailureContext::ForStateComponent(
            "terminalFallbackRequest"));
    callbacks_.noteTheoremAuditViolation(
        formatv("terminal fallback request used generic "
                "MissingProducerFacts without structured context: "
                "stage={0} detail={1}",
                stage, stringutils::showWsWithClip(detail, 200))
            .str());
  }

  const std::optional<TheoremFallbackFailureKind> normalizedFailure =
      NormalizeTerminalFallbackFailureReason(failure.reason);
  if (!normalizedFailure || *normalizedFailure != failure.theoremFailure) {
    failure = MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied,
        TerminalFallbackFailureReason::TheoremAuditInvariantViolation,
        TerminalFallbackFailureContext::ForStateComponent(
            "terminalFallbackRequest"));
    callbacks_.noteTheoremAuditViolation(
        formatv("terminal fallback request carried inconsistent theorem "
                "failure normalization: stage={0} detail={1}",
                stage, stringutils::showWsWithClip(detail, 200))
            .str());
  }

  requests_.push_back(request);

  callbacks_.traceTerminalRequest(request);
  REFOLD_LOG_DEBUG("fallback", "{0}", request);
}

} // namespace refold
} // namespace clang

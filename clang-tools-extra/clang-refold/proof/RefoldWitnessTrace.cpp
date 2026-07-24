//===--- RefoldWitnessTrace.cpp ---------------------------------*- C++ -*-===//
//
// Witness trace and proof-audit logging implementation.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldWitnessTrace.h"

#include "core/RefoldLog.h"
#include "proof/RefoldWitnessClassifier.h"
#include "util/StringUtils.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <cctype>
#include <cstddef>
#include <optional>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

std::string formatOptionalIndex(std::optional<size_t> index) {
  if (!index)
    return "none";
  return llvm::formatv("{0}", *index).str();
}

std::string formatWitnessClosureAtom(StringRef value) {
  if (value.empty())
    return "<none>";

  std::string out;
  for (char raw : value) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':' ||
        c == '/' || c == '#' || c == '@' || c == '[' || c == ']' || c == '(' ||
        c == ')' || c == ',' || c == '=') {
      out.push_back(static_cast<char>(c));
      continue;
    }

    out += "%";
    const std::string hex = llvm::utohexstr(c, /*LowerCase=*/true);
    if (hex.size() == 1)
      out += "0";
    out += hex;
  }
  return out;
}

template <typename FormatObject> void logProofLine(const FormatObject &line) {
  std::string text = line.str();
  while (!text.empty() && text.back() == '\n')
    text.pop_back();
  REFOLD_LOG_INFO("proof", "{0}", text);
}

} // namespace

RefoldWitnessTrace::RefoldWitnessTrace(bool strict,
                                       const ProofAuditMode &proofAuditMode,
                                       const bool &alignmentSemanticTheoremActive)
    : strict_(strict), proofAuditMode_(proofAuditMode),
      alignmentSemanticTheoremActive_(alignmentSemanticTheoremActive) {}

WitnessResolverMode RefoldWitnessTrace::GetWitnessResolverMode() const {
  if (strict_ || alignmentSemanticTheoremActive_)
    return WitnessResolverMode::Strict;

  switch (proofAuditMode_) {
  case ProofAuditMode::Default:
  case ProofAuditMode::Off:
    return WitnessResolverMode::Off;
  case ProofAuditMode::Probe:
    return WitnessResolverMode::Probe;
  case ProofAuditMode::Strict:
    return WitnessResolverMode::Strict;
  }

  return WitnessResolverMode::Off;
}

bool RefoldWitnessTrace::ShouldEmitProofLog() const {
  return GetWitnessResolverMode() != WitnessResolverMode::Off;
}

std::string RefoldWitnessTrace::FormatWitnessTraceHash(StringRef text) {
  // Fixed FNV-1a keeps traces deterministic without relying on
  // implementation-specific pointer identity or process-local hash seeds.
  uint64_t hash = 14695981039346656037ULL;
  for (char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ULL;
  }
  return std::string("0x") + llvm::utohexstr(hash, /*LowerCase=*/true);
}

void RefoldWitnessTrace::TraceWitnessStrictDomain(
    llvm::StringRef role, const WitnessStrictDomainDecision &decision) const {
  if (!ShouldEmitProofLog() &&
      GetWitnessResolverMode() == WitnessResolverMode::Off)
    return;

  logProofLine(
      llvm::formatv("REFOLD-WITNESS-DOMAIN role={0} domain={1} obligation={2} "
                    "fallback_class={3} reason={4}\n",
                    role, toString(decision.domainClass),
                    toString(decision.obligation), decision.fallbackClass,
                    decision.reason.empty() ? StringRef("<none>")
                                            : StringRef(decision.reason)));
}

void RefoldWitnessTrace::TraceWitnessClosureLedger(
    const WitnessResolverDecision &decision) const {
  if (!ShouldEmitProofLog() && decision.mode == WitnessResolverMode::Off)
    return;

  for (const WitnessClosureLedgerEntry &entry : decision.closureLedger) {
    logProofLine(llvm::formatv(
        "REFOLD-WITNESS-CLOSURE selector={0} family={1} "
        "test_region={2} missing={3} source_family={4} "
        "candidate_kind={5} theorem={6} witness_id={7} "
        "candidate_index={8} candidate_count={9} selectable={10} "
        "complete={11} incomplete={12} converted={13} unconverted={14} "
        "classes={15} owner_kind={16} owner={17} fallback_class={18} "
        "obligation={19} reason={20} missing_reason={21} "
        "composition={22}:classes={23}:complete={24}:incomplete={25}:"
        "incompatible={26}:reason={27}\n",
        formatWitnessClosureAtom(entry.selector), entry.family,
        formatWitnessClosureAtom(entry.testRegion),
        formatWitnessClosureAtom(entry.missingDimension),
        formatWitnessClosureAtom(entry.sourceFamily),
        formatWitnessClosureAtom(entry.candidateKind),
        formatWitnessClosureAtom(entry.theoremClass), entry.witnessId,
        entry.candidateIndex, decision.candidateCount, decision.selectableCount,
        decision.completeWitnessCount, decision.incompleteWitnessCount,
        decision.resolverAuthoritativeWitnessCount,
        decision.resolverUnconvertedWitnessCount,
        decision.equivalenceClassCount,
        formatWitnessClosureAtom(entry.sourceOwnerKind),
        formatWitnessClosureAtom(entry.owner), entry.fallbackClass,
        entry.obligation, formatWitnessClosureAtom(entry.resolverReason),
        formatWitnessClosureAtom(entry.missingReason),
        decision.composition.compatible ? StringRef("compatible")
                                        : StringRef("not-compatible"),
        decision.composition.globalClassCount,
        decision.composition.completeTupleCount,
        decision.composition.incompleteTupleCount,
        decision.composition.incompatibleTupleCount,
        formatWitnessClosureAtom(
            decision.composition.reason.empty()
                ? StringRef("<none>")
                : StringRef(decision.composition.reason))));
  }
}

void RefoldWitnessTrace::TraceWitnessResolverDecision(
    const WitnessResolverDecision &decision) const {
  // Probe and strict modes use the normal proof log as their audit channel.
  if (!ShouldEmitProofLog() && decision.mode == WitnessResolverMode::Off)
    return;

  StringRef authority = "legacy";
  if (decision.mode == WitnessResolverMode::Probe)
    authority = "probe-only";
  else if (decision.mode == WitnessResolverMode::Strict &&
           decision.strictUseResolver)
    authority = "strict-resolver";
  else if (decision.mode == WitnessResolverMode::Strict &&
           decision.strictFailClosed)
    authority = "strict-fail-closed";
  else if (decision.mode == WitnessResolverMode::Strict)
    authority = "strict-fallback-legacy";

  logProofLine(llvm::formatv(
      "REFOLD-WITNESS-RESOLVER role={0} mode={1} candidates={2} "
      "selectable={3} proof_invalid={4} classes={5} complete={6} "
      "incomplete={7} converted={8} unconverted={9} computed={10} "
      "implemented={11} authority={12} legacy_index={13} "
      "resolver_index={14} agreement={15} reason={16} "
      "fallback_class={17} domain={18}:obligation={19}:reason={20} "
      "composition={21}:classes={22}:complete={23}:"
      "incomplete={24}:incompatible={25}:reason={26}\n",
      decision.role, decision.mode, decision.candidateCount,
      decision.selectableCount, decision.proofInvalidCount,
      decision.equivalenceClassCount, decision.completeWitnessCount,
      decision.incompleteWitnessCount,
      decision.resolverAuthoritativeWitnessCount,
      decision.resolverUnconvertedWitnessCount,
      decision.resolverComputed ? 1 : 0, decision.resolverImplemented ? 1 : 0,
      authority, formatOptionalIndex(decision.legacyIndex),
      formatOptionalIndex(decision.resolverIndex),
      decision.agreement.empty() ? StringRef("<none>")
                                 : StringRef(decision.agreement),
      decision.failureReason.empty() ? StringRef("<none>")
                                     : StringRef(decision.failureReason),
      decision.fallbackClass, decision.strictDomain.domainClass,
      decision.strictDomain.obligation,
      decision.strictDomain.reason.empty()
          ? StringRef("<none>")
          : StringRef(decision.strictDomain.reason),
      decision.composition.compatible ? StringRef("compatible")
                                      : StringRef("not-compatible"),
      decision.composition.globalClassCount,
      decision.composition.completeTupleCount,
      decision.composition.incompleteTupleCount,
      decision.composition.incompatibleTupleCount,
      decision.composition.reason.empty()
          ? StringRef("<none>")
          : StringRef(decision.composition.reason)));

  TraceWitnessStrictDomain(decision.role, decision.strictDomain);
  TraceWitnessClosureLedger(decision);
}

void RefoldWitnessTrace::TraceWitnessCompositionDecision(
    llvm::StringRef role, const WitnessCompositionDecision &decision) const {
  if (!ShouldEmitProofLog() &&
      GetWitnessResolverMode() == WitnessResolverMode::Off)
    return;

  logProofLine(llvm::formatv(
      "REFOLD-WITNESS-COMPOSITION role={0} computed={1} tuples={2} "
      "complete={3} incomplete={4} incompatible={5} classes={6} "
      "compatible={7} fatal={8} reason={9}\n",
      role, decision.computed ? 1 : 0, decision.candidateTupleCount,
      decision.completeTupleCount, decision.incompleteTupleCount,
      decision.incompatibleTupleCount, decision.globalClassCount,
      decision.compatible ? 1 : 0, decision.failureIsFatal ? 1 : 0,
      decision.reason.empty() ? StringRef("<none>")
                              : StringRef(decision.reason)));
}

void RefoldWitnessTrace::TraceWitnessEmitted(
    const RefoldWitness &witness) const {
  if (!ShouldEmitProofLog())
    return;

  logProofLine(llvm::formatv("REFOLD-WITNESS {0}\n", witness));
  logProofLine(llvm::formatv("REFOLD-WITNESS-KEY id={0} {1}\n",
                             witness.witnessId, witness.key));
  logProofLine(llvm::formatv("REFOLD-WITNESS-COST id={0} {1}\n",
                             witness.witnessId, witness.cost));
  if (!witness.payloadPreview.empty())
    logProofLine(llvm::formatv("REFOLD-WITNESS-PAYLOAD id={0} text={1}\n",
                               witness.witnessId, witness.payloadPreview));
}

void RefoldWitnessTrace::TraceWitnessRejected(const RefoldWitness &witness,
                                              WitnessRejectReason reason,
                                              llvm::StringRef detail) const {
  if (!ShouldEmitProofLog())
    return;

  logProofLine(llvm::formatv("REFOLD-WITNESS-REJECT id={0} family={1} "
                             "owner={2} reason={3} detail={4}\n",
                             witness.witnessId, witness.family, witness.owner,
                             reason,
                             detail.empty() ? StringRef("<none>") : detail));
}

void RefoldWitnessTrace::TraceWitnessAmbiguity(
    llvm::StringRef role, uint64_t candidateCount, uint64_t selectableCount,
    uint64_t ambiguityClassCount) const {
  if (!ShouldEmitProofLog())
    return;

  logProofLine(llvm::formatv("REFOLD-WITNESS-AMBIGUITY role={0} "
                             "candidates={1} selectable={2} classes={3}\n",
                             role, candidateCount, selectableCount,
                             ambiguityClassCount));
}

void RefoldWitnessTrace::TraceWitnessSelectionProbe(
    llvm::StringRef role, uint64_t candidateCount, uint64_t proofValidCount,
    uint64_t proofInvalidCount, uint64_t equivalenceClassCount,
    uint64_t completeWitnessCount, uint64_t incompleteWitnessCount) const {
  if (!ShouldEmitProofLog())
    return;

  StringRef preferenceScope = "no-valid-witness";
  if (proofValidCount == 1)
    preferenceScope = "single-valid-witness";
  else if (proofValidCount > 1 && equivalenceClassCount == 1)
    preferenceScope = "one-equivalence-class";
  else if (proofValidCount > 1 && equivalenceClassCount > 1)
    preferenceScope = "multiple-equivalence-classes";

  logProofLine(
      llvm::formatv("REFOLD-WITNESS-SELECTION role={0} candidates={1} "
                    "proof_valid={2} proof_invalid={3} classes={4} "
                    "complete={5} incomplete={6} preference_scope={7}\n",
                    role, candidateCount, proofValidCount, proofInvalidCount,
                    equivalenceClassCount, completeWitnessCount,
                    incompleteWitnessCount, preferenceScope));
}

void RefoldWitnessTrace::TraceWitnessChosen(const RefoldWitness &witness,
                                            uint64_t selectedIndex) const {
  if (!ShouldEmitProofLog())
    return;

  logProofLine(llvm::formatv("REFOLD-WITNESS-CHOOSE index={0} id={1} "
                             "family={2} owner={3} cost={4}\n",
                             selectedIndex, witness.witnessId, witness.family,
                             witness.owner, witness.cost));
}

void RefoldWitnessTrace::TraceWitnessFallback(
    const TerminalFallbackRequest &request) const {
  if (!ShouldEmitProofLog())
    return;

  const WitnessFallbackClass fallbackClass =
      classifyTerminalFallbackFailure(request.failure);
  const WitnessStrictDomainDecision domain =
      classifyStrictDomainForTerminalFallback(request.failure);

  logProofLine(llvm::formatv(
      "REFOLD-WITNESS-FALLBACK reason={0} "
      "fallback_class={1} domain={2} obligation={3} "
      "domain_reason={4} stage={5} detail={6}\n",
      request.failure, fallbackClass, domain.domainClass, domain.obligation,
      domain.reason.empty() ? StringRef("<none>") : StringRef(domain.reason),
      request.stage, stringutils::showWsWithClip(request.detail, 200)));
  TraceWitnessStrictDomain("TerminalFallback", domain);

  if (domain.domainClass ==
      WitnessStrictDomainClass::PotentiallyInDomainMissingProof) {
    StringRef missing = "unknown";
    switch (fallbackClass) {
    case WitnessFallbackClass::UnknownTargetPreprocessedTokens:
      missing = "target_pp";
      break;
    case WitnessFallbackClass::UnknownSuffixState:
      missing = "suffix_state";
      break;
    case WitnessFallbackClass::UnprovenProducerKind:
    case WitnessFallbackClass::NoSelectableWitness:
      missing = "producer";
      break;
    case WitnessFallbackClass::CounterStateMismatch:
      missing = "counter";
      break;
    case WitnessFallbackClass::LineControlObserverMismatch:
      missing = "observers";
      break;
    case WitnessFallbackClass::CompositionFailure:
      missing = "composition";
      break;
    case WitnessFallbackClass::UnconvertedProofFamily:
      missing = "converted_family";
      break;
    case WitnessFallbackClass::IncompleteWitnessKey:
      missing = "complete_key";
      break;
    case WitnessFallbackClass::NoOwnerClosedWitness:
      missing = "owner_closure";
      break;
    case WitnessFallbackClass::ValidationFailure:
      missing = "diagnostics";
      break;
    case WitnessFallbackClass::Unknown:
    case WitnessFallbackClass::MultipleNonEquivalentWitnessClasses:
    case WitnessFallbackClass::InvalidMacroInvocation:
    case WitnessFallbackClass::InvalidPasteResult:
    case WitnessFallbackClass::UnsupportedDirectiveInteraction:
      break;
    }

    logProofLine(llvm::formatv(
        "REFOLD-WITNESS-CLOSURE selector=TerminalFallback "
        "family={0} test_region=terminal missing={1} "
        "source_family=TerminalFallback candidate_kind=TerminalOutOfDomain "
        "theorem=TerminalFallback witness_id=0 candidate_index=0 "
        "candidate_count=1 selectable=0 complete=0 incomplete=1 "
        "converted=0 unconverted=1 classes=0 owner_kind=terminal "
        "owner=terminal fallback_class={2} obligation={3} reason={4} "
        "missing_reason={5} "
        "composition=not-compatible:classes=0:complete=0:incomplete=1:"
        "incompatible=0:reason={6}\n",
        WitnessProofFamily::TerminalFallback, formatWitnessClosureAtom(missing),
        fallbackClass, domain.obligation,
        formatWitnessClosureAtom(domain.reason.empty()
                                     ? StringRef("<none>")
                                     : StringRef(domain.reason)),
        formatWitnessClosureAtom(request.failure.ToString()),
        formatWitnessClosureAtom(request.stage.empty()
                                     ? StringRef("<none>")
                                     : StringRef(request.stage))));
  }
}

} // namespace refold
} // namespace clang

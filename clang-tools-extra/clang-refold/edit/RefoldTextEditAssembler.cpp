//===--- RefoldTextEditAssembler.cpp ---------------------------*- C++ -*-===//
//
// Final text-edit assembly and accepted-result audit attachment.
//
// This file implements the final byte-edit assembler: pending line-resync
// application, accepted-result carrier attachment/auditing, sideband replay range
// stamping, and materialized edit-map range recovery.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldLog.h"
#include "edit/RefoldTextEditAssembler.h"
#include "edit/RefoldTUEditPlanner.h"
#include "include/IncludeSpellingHelpers.h"
#include "include/RefoldIncludeReplayProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineControlEditHelpers.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroStateProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTerminalProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "util/RefoldDenseMapInfo.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldTextEditAssembler::ResyncOutcome
RefoldTextEditAssembler::ApplyResyncOrPend(StringRef originalFileText, uint64_t start,
                                uint64_t end, StringRef replacement,
                                StringRef fileSpellingForDirective,
                                std::optional<uint64_t> ownerIncludeId) const {
  // If the replacement preserves the original newline count, no line-state
  // correction is needed.
  size_t origNl = stringutils::countNewlines(originalFileText, start, end);
  size_t replNl =
      stringutils::countNewlines(replacement, 0, replacement.size());
  if (origNl == replNl)
    return ResyncOutcome(replacement.str(), std::nullopt);

  // Newline drift is only observable when a preserved suffix builtin depends
  // on the logical line component. Do not synthesize #line directives merely
  // because physical newline counts changed: materialized __LINE__ values do
  // not observe the stream, and preserved __FILE__/__FILE_NAME__ only observe
  // the file component, which newline drift alone does not change.
  LineStateObserverDemand demand = lineControlProof_.OwnerSuffixLineStateObserverDemand(
      ownerIncludeId, fileSpellingForDirective, end);
  const OwnerStateBoundary suffixBoundary =
      OwnerStateBoundary::FromSource(OwnerSourceRange::From(
          fileSpellingForDirective, end, end, ownerIncludeId));
  const bool resyncPruneEligible = demand.PrunableByCompactFinalLineControl();

  auto checkLineControlStateWithWitness =
      [&](OwnerStateComponent component, StateMutationKind mutation,
          SuffixStabilityWitness witness, StringRef detail,
          bool requireKnownObserver) {
        return ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
            suffixBoundary, component, mutation, std::move(witness),
            "linedir/resync", detail, requireKnownObserver);
      };

  auto checkLineControlStateRepaired =
      [&](OwnerStateComponent component, StateMutationKind mutation,
          StringRef detail, bool requireKnownObserver = false) {
        return checkLineControlStateWithWitness(
            component, mutation,
            ownerStateProof_.BuildStateTransitionWitness(SuffixStabilityWitnessKind::StateRepair,
                                        component, suffixBoundary, detail),
            detail, requireKnownObserver);
      };

  auto checkLineControlStateTerminal =
      [&](OwnerStateComponent component, StateMutationKind mutation,
          StringRef detail, bool requireKnownObserver = true) {
        return checkLineControlStateWithWitness(
            component, mutation,
            ownerStateProof_.BuildStateTransitionWitness(
                SuffixStabilityWitnessKind::TerminalStateFailure, component,
                suffixBoundary, detail),
            detail, requireKnownObserver);
      };

  if (!demand.needsLine) {
    return ResyncOutcome(replacement.str(), std::nullopt);
  }

  // A preserved suffix __LINE__ would otherwise resume at a different logical
  // line. Use the source-authored line-control state at `end`, not merely the
  // physical line in this owner file: `#line`, `# line`, and `# <number>` can
  // make a copied suffix observe a virtual file/line.
  LineDirectiveLocation resumeLoc =
      LineDirectiveInserter::LogicalLocationAtOffset(
          originalFileText, end, fileSpellingForDirective, model_,
          fileSpellingForDirective, ownerIncludeId);

  if (!resumeLoc.producerProven) {
    if (resumeLoc.unprovenLineControlDirectiveOffset &&
        *resumeLoc.unprovenLineControlDirectiveOffset >= start &&
        *resumeLoc.unprovenLineControlDirectiveOffset < end) {
      (void)checkLineControlStateTerminal(
          OwnerStateComponent::LineNumber, StateMutationKind::Consumed,
          llvm::formatv(
              "newline-drift replacement consumes unmodeled source #line "
              "directive at byte {0} in owner '{1}'",
              *resumeLoc.unprovenLineControlDirectiveOffset,
              fileSpellingForDirective)
              .str(),
          /*requireKnownObserver=*/true);
      return ResyncOutcome(replacement.str(), std::nullopt);
    }

    if (std::optional<LineDirectiveLocation> producerLoc =
            lineControlProof_.ProducerBackedLineControlLocationAt(originalFileText,
                                                fileSpellingForDirective,
                                                ownerIncludeId, start, end)) {
      resumeLoc = std::move(*producerLoc);
    } else {
      return ResyncOutcome(replacement.str(), std::nullopt);
    }
  }

  const bool deferToConditionalJoin = hooks_.lineResyncShouldDeferToConditionalJoin(
      fileSpellingForDirective, ownerIncludeId, end);
  if (deferToConditionalJoin) {
    (void)checkLineControlStateRepaired(
        OwnerStateComponent::LineNumber, StateMutationKind::MovedLater,
        llvm::formatv("deferred synthetic #line repair for newline drift at "
                      "owner byte {0}",
                      end)
            .str(),
        /*requireKnownObserver=*/false);
    return ResyncOutcome{replacement.str(),
                         PendingResync{fileSpellingForDirective, ownerIncludeId,
                                       resyncPruneEligible,
                                       resumeLoc.fileSpelling, resumeLoc.lineNo,
                                       end,
                                       /*deferToJoin=*/true}};
  }

  std::string injected = lineDirs_.MaybeAppendResyncAfterReplacement(
      originalFileText, start, end, replacement, resumeLoc);
  const std::string resyncDirective =
      lineDirs_.FormatLineDirective(resumeLoc.lineNo, resumeLoc.fileSpelling);

  // A changed result means the directive was inserted directly into this
  // replacement, so no deferred resync state needs to be carried forward.
  if (injected != replacement) {
    (void)checkLineControlStateRepaired(
        OwnerStateComponent::LineNumber, StateMutationKind::Replayed,
        llvm::formatv("local synthetic #line repair for newline drift at "
                      "owner byte {0}",
                      end)
            .str(),
        /*requireKnownObserver=*/false);

    std::vector<FinalLineControlPruneCandidate> candidates;
    if (resyncPruneEligible) {
      if (std::optional<FinalLineControlPruneCandidate> candidate =
              makeInsertedSyntheticLineControlPruneCandidate(
                  replacement, injected, resyncDirective,
                  FinalLineDirective::Origin::SyntheticNewlineResync,
                  FinalLineControlOwnerKey(fileSpellingForDirective.str(),
                                           ownerIncludeId),
                  FinalLineControlObligation::CosmeticSyntheticResync)) {
        candidates.push_back(std::move(*candidate));
      }
    }

    return ResyncOutcome(std::move(injected), std::nullopt,
                         std::move(candidates));
  }

  // Local injection was not safe, usually because the replacement rejoins
  // untouched bytes mid-line. Carry a pending resync so the next copied
  // original slice can emit the directive at a valid boundary.
  (void)checkLineControlStateRepaired(
      OwnerStateComponent::LineNumber, StateMutationKind::MovedLater,
      llvm::formatv("pending synthetic #line repair for newline drift at "
                    "owner byte {0}",
                    end)
          .str(),
      /*requireKnownObserver=*/false);

  return ResyncOutcome{replacement.str(),
                       PendingResync{fileSpellingForDirective, ownerIncludeId,
                                     resyncPruneEligible,
                                     resumeLoc.fileSpelling, resumeLoc.lineNo,
                                     end}};
}

void RefoldTextEditAssembler::AttachAcceptedResultCarrier(
    TextEdit &edit, const AcceptedResultCandidate &candidate) const {
  theoremAuditService_.AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "AttachAcceptedResultCarrier");
  edit.acceptedResults.push_back(
      std::make_shared<AcceptedResultCandidate>(candidate));
}

bool RefoldTextEditAssembler::AuditAcceptedEditProofs(ArrayRef<TextEdit> edits,
                                          StringRef emissionStage,
                                          StringRef emissionOwner) const {
  // Centralize the final accepted-proof audit at the last byte-edit boundary.
  // Earlier builders may still stage candidates path-by-path, but once the
  // normalized edit set is known the applicator must see a theorem carrier for
  // every emitted edit and a composition law for every multi-carrier edit.  The
  // state-transition audit is layered onto the same boundary:
  // any edit path that already routed state through the gateway must have a
  // typed, component-named suffix-stability witness or a named terminal failure
  // before non-terminal bytes can be emitted.
  if (!theoremAuditService_.AuditStateTransitionGatewayProofs(emissionStage, emissionOwner))
    return false;

  for (const TextEdit &edit : edits) {
    if (!EmittedTextEditHasDischargedAcceptedResults(edit, emissionStage,
                                                     emissionOwner))
      return false;
  }
  return true;
}

bool RefoldTextEditAssembler::EmittedTextEditHasDischargedAcceptedResults(
    const TextEdit &edit, StringRef emissionStage,
    StringRef emissionOwner) const {
  const StringRef owner =
      emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner;
  ++theoremAudit_.emittedNonTerminalEdits;

  auto carrierIsEmissionDischarged =
      [&](const AcceptedResultCandidate &carrier) -> bool {
    ++theoremAudit_.emittedCarriers;
    theoremAuditService_.AuditAcceptedResultCandidateForLegacyAuthority(
        carrier, "EmittedTextEditHasDischargedAcceptedResults");

    // Non-terminal byte edits must never carry terminal/out-of-domain results.
    // After these structural exclusions, the single normalizer is the only
    // authority for deciding whether the candidate's construction path and
    // proof-family metadata discharge one final theorem proof class.
    if (carrier.kind == AcceptedResultCandidateKind::Unknown) {
      ++theoremAudit_.emittedUnknownClassCarriers;
      theoremAuditService_.NoteTheoremAuditViolation("emitted carrier had unknown candidate kind");
      return false;
    }
    if (carrier.kind == AcceptedResultCandidateKind::TerminalOutOfDomain) {
      ++theoremAudit_.emittedOutOfDomainCarriers;
      theoremAuditService_.NoteTheoremAuditViolation(
          "non-terminal emitted edit carried explicit out-of-domain result");
      return false;
    }

    const std::optional<TheoremProofClass> theoremProof =
        proofLattice_.NormalizeAcceptedProof(carrier);
    if (!theoremProof) {
      if (carrier.proofSummary.theoremClass == TheoremProofClass::Unknown) {
        ++theoremAudit_.emittedUnknownClassCarriers;
        theoremAuditService_.NoteTheoremAuditViolation(
            "emitted carrier did not declare a final theorem proof class");
      } else if (carrier.proofSummary.inventory.support !=
                     AcceptanceSupportKind::ExplicitProofBacked ||
                 carrier.proofSummary.theoremDomain.kind ==
                     TheoremDomainKind::TransitionalGap ||
                 carrier.proofSummary.completeness.coverage ==
                     CompletenessCoverageKind::TransitionalGap) {
        ++theoremAudit_.emittedTransitionalTheoremCarriers;
        theoremAuditService_.NoteTheoremAuditViolation(
            "emitted carrier remained transitional at the byte-edit boundary");
      } else {
        theoremAuditService_.NoteTheoremAuditViolation(
            "emitted carrier failed final proof normalization at the "
            "byte-edit boundary");
      }
      ++theoremAudit_.emittedUndischargedCarriers;
      return false;
    }

    ++theoremAudit_.emittedDeclaredClassCarriers;
    ++theoremAudit_.emittedDischargedCarriers;
    REFOLD_LOG_TRACE("proof/normalize",
          "emitted carrier normalized theoremProof={0} kind={1} path={2}",
          *theoremProof, carrier.kind,
          carrier.proofSummary.inventory.currentPath);
    return true;
  };

  // Every emitted non-terminal edit must carry at least one selected
  // accepted-result witness. An unannotated edit is an emission artifact, not a
  // theorem-facing refolding result.
  if (edit.acceptedResults.empty()) {
    const TerminalFallbackProofFailure failure =
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionArtifactDischarged,
            TerminalFallbackFailureReason::UndischargedEmissionArtifact,
            TerminalFallbackFailureContext::ForStateComponent(
                "TextEdit.acceptedResults"));
    const bool strictRejected = theoremAuditService_.RejectNoLegacyAuditFindingIfStrict(
        RefoldTheoremAudit::MakeLegacyAuditEvidence(
            LegacyPathKind::PathSpecificProofMirror, emissionStage,
            llvm::formatv(
                "emitted edit bytes=[{0},{1}) in {2} has no "
                "AcceptedResultCandidate / ProofSummary carrier",
                edit.start, edit.end, owner)
                .str()),
        failure);
    theoremAuditService_.NoteTheoremAuditViolation("emitted edit reached the byte-edit boundary "
                              "without accepted-result carriers");
    if (!strictRejected) {
      terminalSink_.RequestTerminalFallback(
          failure, emissionStage,
          llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has no "
                        "normalized accepted-result carriers",
                        edit.start, edit.end, owner)
              .str());
    }
    return false;
  }

  // Validate every carrier attached to the edit. Multiple carriers are allowed
  // when one concrete edit composes several accepted results, but each carrier
  // must independently discharge.
  for (size_t i = 0; i < edit.acceptedResults.size(); ++i) {
    const std::shared_ptr<const AcceptedResultCandidate> &carrier =
        edit.acceptedResults[i];
    if (!carrier) {
      const TerminalFallbackProofFailure failure =
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact,
              TerminalFallbackFailureContext::ForStateComponent(
                  "TextEdit.acceptedResults.null"));
      const bool strictRejected = theoremAuditService_.RejectNoLegacyAuditFindingIfStrict(
          RefoldTheoremAudit::MakeLegacyAuditEvidence(
              LegacyPathKind::PathSpecificProofMirror, emissionStage,
              llvm::formatv(
                  "emitted edit bytes=[{0},{1}) in {2} has null "
                  "AcceptedResultCandidate carrier #{3}",
                  edit.start, edit.end, owner, i)
                  .str()),
          failure);
      theoremAuditService_.NoteTheoremAuditViolation(
          "emitted edit carried a null accepted-result carrier");
      if (!strictRejected) {
        terminalSink_.RequestTerminalFallback(
            failure, emissionStage,
            llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has null "
                          "accepted-result carrier #{3}",
                          edit.start, edit.end, owner, i)
                .str());
      }
      return false;
    }

    if (!carrierIsEmissionDischarged(*carrier)) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          emissionStage,
          llvm::formatv("emitted edit bytes=[{0},{1}) in {2} carries "
                        "non-discharged result #{3} kind={4}",
                        edit.start, edit.end, owner, i, carrier->kind)
              .str());
      return false;
    }
  }

  return EmittedTextEditHasOrderedAcceptedProofComposition(
      edit, emissionStage, emissionOwner);
}

bool RefoldTextEditAssembler::EmittedTextEditHasOrderedAcceptedProofComposition(
    const TextEdit &edit, StringRef emissionStage,
    StringRef emissionOwner) const {
  if (edit.acceptedResults.size() <= 1)
    return true;

  ++theoremAudit_.emittedCompositeEdits;

  enum class CarrierSpanDomain : uint8_t { AToken, TUByte };
  struct CarrierSpan {
    CarrierSpanDomain domain;
    uint64_t begin = 0;
    uint64_t end = 0;
    size_t carrierIndex = 0;
    TheoremProofClass theoremProof = TheoremProofClass::Unknown;
  };

  auto domainName = [](CarrierSpanDomain domain) -> StringRef {
    switch (domain) {
    case CarrierSpanDomain::AToken:
      return "AToken";
    case CarrierSpanDomain::TUByte:
      return "TUByte";
    }
    return "Unknown";
  };

  const StringRef owner =
      emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner;

  auto failComposition = [&](StringRef detail) -> bool {
    ++theoremAudit_.emittedUncomposedCompositeEdits;
    theoremAuditService_.NoteTheoremAuditViolation(detail);
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionArtifactDischarged,
            TerminalFallbackFailureReason::UndischargedEmissionArtifact),
        emissionStage,
        llvm::formatv("emitted composite edit bytes=[{0},{1}) in {2}: {3}",
                      edit.start, edit.end, owner, detail)
            .str());
    return false;
  };

  auto extractSpan = [&](const AcceptedResultCandidate &carrier,
                         size_t carrierIndex,
                         TheoremProofClass theoremProof)
      -> std::optional<CarrierSpan> {
    CarrierSpan span;
    span.carrierIndex = carrierIndex;
    span.theoremProof = theoremProof;

    switch (carrier.kind) {
    case AcceptedResultCandidateKind::MacroPatch:
    case AcceptedResultCandidateKind::IncludePatch:
      // Macro and include carriers record A-side preprocessed-token envelopes.
      // They are comparable to one another for proof-composition purposes, but
      // not to the physical byte range of the final TextEdit.
      span.domain = CarrierSpanDomain::AToken;
      span.begin = carrier.begin;
      span.end = carrier.end;
      break;

    case AcceptedResultCandidateKind::TUAnchor:
      if (carrier.proofSummary.hasTUAnchorWitness &&
          carrier.proofSummary.tuAnchorWitness.hasPPGap) {
        span.domain = CarrierSpanDomain::AToken;
        span.begin = span.end = carrier.proofSummary.tuAnchorWitness.ppGap;
      } else if (carrier.hasAnchorByte) {
        span.domain = CarrierSpanDomain::TUByte;
        span.begin = span.end = carrier.anchorByte;
      } else {
        return std::nullopt;
      }
      break;

    case AcceptedResultCandidateKind::TUTextEdit:
      span.domain = CarrierSpanDomain::TUByte;
      span.begin = carrier.begin;
      span.end = carrier.end;
      break;

    case AcceptedResultCandidateKind::TerminalOutOfDomain:
    case AcceptedResultCandidateKind::Unknown:
      return std::nullopt;
    }

    if (span.end < span.begin)
      return std::nullopt;
    return span;
  };

  SmallVector<CarrierSpan, 8> spans;
  spans.reserve(edit.acceptedResults.size());
  for (size_t i = 0; i < edit.acceptedResults.size(); ++i) {
    const std::shared_ptr<const AcceptedResultCandidate> &carrierPtr =
        edit.acceptedResults[i];
    if (!carrierPtr)
      return failComposition("composite edit carried a null proof segment");

    const std::optional<TheoremProofClass> theoremProof =
        proofLattice_.NormalizeAcceptedProof(*carrierPtr);
    if (!theoremProof)
      return failComposition(
          "composite edit contained a carrier that did not normalize");

    std::optional<CarrierSpan> span =
        extractSpan(*carrierPtr, i, *theoremProof);
    if (!span)
      return failComposition(
          llvm::formatv("composite carrier #{0} had no comparable proof span "
                        "kind={1}",
                        i, carrierPtr->kind)
              .str());
    spans.push_back(*span);
  }

  // Several construction paths can converge on the exact same concrete source
  // surface.  Those carriers are equivalent witnesses for one segment rather
  // than a sequence of multiple segments, so no ordering/gap proof is needed.
  bool sameSurface = true;
  for (size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].domain != spans[0].domain ||
        spans[i].begin != spans[0].begin || spans[i].end != spans[0].end) {
      sameSurface = false;
      break;
    }
  }
  if (sameSurface) {
    ++theoremAudit_.emittedEquivalentCompositeEdits;
    REFOLD_LOG_TRACE("proof/compose",
          "composite edit bytes=[{0},{1}) in {2} has {3} equivalent "
          "proof carriers over {4}[{5},{6})",
          edit.start, edit.end, owner, spans.size(),
          domainName(spans[0].domain), spans[0].begin, spans[0].end);
    return true;
  }

  // Duplicate physical edits can merge a TU-byte carrier with owner-local
  // A-token carriers.  When one normalized carrier proves the exact physical
  // emitted byte surface, the non-TU carriers are redundant witnesses rather
  // than additional ordered segments.  This preserves existing deterministic
  // duplicate-edit behavior without treating cross-domain carrier order as a
  // proof.
  for (const CarrierSpan &span : spans) {
    if (span.domain == CarrierSpanDomain::TUByte && span.begin == edit.start &&
        span.end == edit.end) {
      ++theoremAudit_.emittedEquivalentCompositeEdits;
      REFOLD_LOG_TRACE("proof/compose",
            "composite edit bytes=[{0},{1}) in {2} is dominated by carrier "
            "#{3} theoremProof={4} over TUByte[{5},{6}); {7} auxiliary "
            "carriers remain individually discharged",
            edit.start, edit.end, owner, span.carrierIndex, span.theoremProof,
            span.begin, span.end, spans.size() - 1);
      return true;
    }
  }

  // Same-point insertion composition consumes no A-side bytes/tokens.  Multiple
  // zero-width carriers may therefore share one physical insertion edit without
  // needing an inter-segment source-gap proof; the emitted text order was fixed
  // by the duplicate-insertion merge that built this TextEdit.
  const bool allZeroWidth = llvm::all_of(spans, [](const CarrierSpan &span) {
    return span.begin == span.end;
  });
  if (allZeroWidth && edit.start == edit.end) {
    ++theoremAudit_.emittedEquivalentCompositeEdits;
    REFOLD_LOG_TRACE("proof/compose",
          "composite insertion edit bytes=[{0},{1}) in {2} has {3} "
          "zero-width proof carriers",
          edit.start, edit.end, owner, spans.size());
    return true;
  }

  // Ordered segment composition is only meaningful inside one coordinate space.
  // Mixing A-token carriers with TU-byte carriers would require a separate
  // cross-domain owner-closure witness, so the proof model rejects that
  // composition instead of silently treating the carrier vector as an unordered
  // bag.
  for (size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].domain != spans[0].domain) {
      return failComposition(
          llvm::formatv(
              "carrier #{0} uses {1} coordinates after {2} coordinates; "
              "no cross-domain composition witness is attached",
              spans[i].carrierIndex, domainName(spans[i].domain),
              domainName(spans[0].domain))
              .str());
    }
  }

  // The carrier list itself must be ordered.  Sorting here would hide a missing
  // construction proof, so the audit checks the preserved order and rejects
  // overlaps or non-empty source gaps.  Future state-gap proof extensions can
  // relax the gap rule by attaching typed state-closed gap witnesses.
  for (size_t i = 1; i < spans.size(); ++i) {
    const CarrierSpan &prev = spans[i - 1];
    const CarrierSpan &cur = spans[i];
    if (cur.begin < prev.end) {
      return failComposition(
          llvm::formatv("carrier #{0} span {1}[{2},{3}) overlaps previous "
                        "carrier #{4} span {1}[{5},{6})",
                        cur.carrierIndex, domainName(cur.domain), cur.begin,
                        cur.end, prev.carrierIndex, prev.begin, prev.end)
              .str());
    }
    if (cur.begin > prev.end) {
      return failComposition(
          llvm::formatv("carrier #{0} span {1}[{2},{3}) leaves non-empty "
                        "unproved gap after carrier #{4} span {1}[{5},{6}); "
                        "current composition proof admits only empty gaps until "
                        "state-gap witnesses are available",
                        cur.carrierIndex, domainName(cur.domain), cur.begin,
                        cur.end, prev.carrierIndex, prev.begin, prev.end)
              .str());
    }
  }

  // If the carrier coordinates are physical TU bytes, they must tile the actual
  // source interval being emitted.  A-token carriers prove their own PP
  // envelope through the normalized per-carrier proof; the final TextEdit byte
  // span is a separately mapped owner surface and is not directly comparable
  // here.
  if (spans[0].domain == CarrierSpanDomain::TUByte &&
      (spans.front().begin != edit.start || spans.back().end != edit.end)) {
    return failComposition(
        llvm::formatv("TU-byte composite carriers tile [{0},{1}) but emitted "
                      "edit covers [{2},{3})",
                      spans.front().begin, spans.back().end, edit.start,
                      edit.end)
            .str());
  }

  ++theoremAudit_.emittedOrderedCompositeEdits;
  REFOLD_LOG_TRACE("proof/compose",
        "composite edit bytes=[{0},{1}) in {2} has {3} ordered contiguous "
        "proof segments in {4} coordinates",
        edit.start, edit.end, owner, spans.size(), domainName(spans[0].domain));
  return true;
}


void RefoldTextEditAssembler::StampTextEditMaterializedBByteRange(TextEdit &edit,
                                                       uint64_t begin,
                                                       uint64_t end) const {
  if (end < begin || end > static_cast<uint64_t>(bSource_.size()))
    REFOLD_LOG_FATAL("edit-map", "invalid materialized B byte range [{0},{1}) bLen={2}",
          begin, end, bSource_.size());
  edit.materializedBByteBegin = begin;
  edit.materializedBByteEnd = end;
}

void RefoldTextEditAssembler::StampTextEditMaterializedBTokenRange(
    TextEdit &edit, uint64_t bTokBegin, uint64_t bTokEnd) const {
  std::optional<std::pair<uint64_t, uint64_t>> bytes =
      sourceMapper_.BTokenRangeToByteRange(bTokBegin, bTokEnd);
  if (!bytes)
    REFOLD_LOG_FATAL("edit-map", "invalid materialized B token range [{0},{1}) bToks={2}",
          bTokBegin, bTokEnd, bToks_.size());
  StampTextEditMaterializedBByteRange(edit, bytes->first, bytes->second);
}

void RefoldTextEditAssembler::StampTextEditMaterializedBReplayProof(
    TextEdit &edit, const SidebandPragmaEdit &sideband) const {
  const std::pair<uint64_t, uint64_t> bRange =
      sideband.MaterializedBByteRange();
  StampTextEditMaterializedBByteRange(edit, bRange.first, bRange.second);

  const std::pair<uint64_t, uint64_t> outputRange =
      sideband.MaterializedOutputTextRange();
  StampTextEditMaterializedOutputTextRange(edit, outputRange.first,
                                           outputRange.second);
}

std::string RefoldTextEditAssembler::StripSeparatelyOwnedSidebandReplay(
    StringRef replayText, std::optional<uint64_t> replayBByteBegin,
    std::optional<uint64_t> replayBByteEnd) const {
  return stripSeparatelyOwnedSidebandReplay(sidebandPragmaEdits_, replayText,
                                            replayBByteBegin, replayBByteEnd);
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::SidebandPragmaMaterializedBByteRangeForInclude(
    uint64_t includeId) const {
  return sidebandPragmaMaterializedBByteRangeForInclude(
      sidebandPragmaEdits_, includeId, static_cast<uint64_t>(bSource_.size()));
}

void RefoldTextEditAssembler::StampTextEditMaterializedOutputTextRange(
    TextEdit &edit, uint64_t begin, uint64_t end) const {
  if (end < begin || end > static_cast<uint64_t>(edit.text.size()))
    REFOLD_LOG_FATAL("edit-map",
          "invalid materialized output byte range [{0},{1}) textLen={2}",
          begin, end, edit.text.size());
  edit.materializedOutputTextBegin = begin;
  edit.materializedOutputTextEnd = end;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::TextEditMaterializedOutputTextRange(const TextEdit &edit) const {
  if (edit.materializedOutputTextBegin && edit.materializedOutputTextEnd) {
    if (*edit.materializedOutputTextEnd < *edit.materializedOutputTextBegin ||
        *edit.materializedOutputTextEnd >
            static_cast<uint64_t>(edit.text.size()))
      return std::nullopt;
    return std::make_pair(*edit.materializedOutputTextBegin,
                          *edit.materializedOutputTextEnd);
  }

  return std::make_pair(uint64_t{0}, static_cast<uint64_t>(edit.text.size()));
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::MacroPatchMaterializedOutputTextRange(
    const MacroPatch &patch) const {
  if (!patch.hasMaterializedOutputByteRange)
    return std::nullopt;
  if (patch.materializedOutputByteEnd < patch.materializedOutputByteStart ||
      patch.materializedOutputByteEnd >
          static_cast<uint64_t>(patch.replacement.size()))
    return std::nullopt;
  return std::make_pair(patch.materializedOutputByteStart,
                        patch.materializedOutputByteEnd);
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::TextEditMaterializedBByteRange(const TextEdit &edit) const {
  if (edit.materializedBByteBegin && edit.materializedBByteEnd) {
    if (*edit.materializedBByteEnd < *edit.materializedBByteBegin ||
        *edit.materializedBByteEnd > static_cast<uint64_t>(bSource_.size()))
      return std::nullopt;
    return std::make_pair(*edit.materializedBByteBegin,
                          *edit.materializedBByteEnd);
  }

  if (edit.directTUHunkBStart && edit.directTUHunkBEnd)
    return sourceMapper_.BTokenRangeToByteRange(
        *edit.directTUHunkBStart, *edit.directTUHunkBEnd);

  return std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::MacroPatchMaterializedBByteRange(const MacroPatch &patch) const {
  // Prefer an explicitly stamped envelope.  Structure-preserving macro patches
  // use this to distinguish two edit-map meanings that share the same physical
  // TextEdit: ordinary argument rewrites map the B side to the whole expansion
  // envelope they regenerate, while pure insertions intentionally stay narrow
  // and let the final TextEdit/hunk metadata provide the inserted payload.
  if (patch.hasMaterializedBTokenRange) {
    if (auto bytes = sourceMapper_.BTokenRangeToByteRange(
            patch.materializedBTokStart, patch.materializedBTokEnd))
      return bytes;
  }

  uint64_t macroId = patch.proof.proofRootMacroId ? patch.proof.proofRootMacroId
                                            : patch.macroId;
  if (macroId == 0)
    return std::nullopt;

  const RefoldModel::MacroInvocation *macro = macroTopology_.FindMacroInvocationById(macroId);
  if (!macro)
    return std::nullopt;

  if (auto plan = hooks_.computeWholeCoverPlan(*macro))
    return sourceMapper_.BTokenRangeToByteRange(plan->bTokStart, plan->bTokEnd);

  if (auto env =
          sourceMapper_.MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
              macro->cover.begin, macro->cover.end))
    return sourceMapper_.BTokenRangeToByteRange(env->first, env->second);

  return std::nullopt;
}

std::string RefoldTextEditAssembler::ApplyTextEditsWithPendingResync(
    StringRef originalFileText, ArrayRef<TextEdit> edits,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds,
    StringRef emissionOwner, std::optional<uint64_t> ownerIncludeId,
    std::vector<MaterializedEditMapping> *materializedEditMappings,
    std::vector<FinalLineControlPruneCandidate>
        *lineControlPruneCandidates,
    std::vector<FinalLineControlSourceMapping>
        *lineControlSourceMappings) const {
  // Source-side ranges are only knowable at this final splice boundary: every
  // earlier candidate still may be merged, dropped, widened, or followed by a
  // pending resync before it becomes physical output.
  if (materializedEditMappings)
    materializedEditMappings->clear();
  if (lineControlPruneCandidates)
    lineControlPruneCandidates->clear();
  if (lineControlSourceMappings)
    lineControlSourceMappings->clear();
  const std::string sourceMappingOwner =
      emissionOwner.empty() ? std::string()
                            : lineDirs_.ToAbsolutePath(emissionOwner);
  if (edits.empty()) {
    if (lineControlSourceMappings && !originalFileText.empty() &&
        !sourceMappingOwner.empty()) {
      lineControlSourceMappings->push_back(FinalLineControlSourceMapping{
          0, static_cast<uint64_t>(originalFileText.size()),
          sourceMappingOwner, 0,
          static_cast<uint64_t>(originalFileText.size()), ownerIncludeId});
    }
    return originalFileText.str();
  }

  // Normalize edits by span.
  //
  // Historically we deduped by (start,end) using a map, which accidentally
  // dropped legitimate *multiple insertions* at the same byte offset.
  // This happens when the diff produces adjacent insert-only hunks (e.g. when
  // comments tokenize separately), and both map to the same insertion point.
  //
  // We now:
  //  * Preserve and concatenate multiple INSERT edits with identical
  //    zero-length spans ([x,x)) in their original order.
  //  * Merge duplicate non-zero edits only when they carry the same
  //    replacement payload; conflicting duplicate replacements request the
  //    explicit terminal fallback instead of selecting a writer by order.

  struct EditRef {
    const TextEdit *e;
    size_t idx;
  };

  SmallVector<EditRef, 32> ordered;
  ordered.reserve(edits.size());
  for (size_t i = 0; i < edits.size(); ++i)
    ordered.push_back(EditRef{&edits[i], i});

  sort(ordered, [](const EditRef &a, const EditRef &b) {
    if (a.e->start != b.e->start)
      return a.e->start < b.e->start;
    if (a.e->end != b.e->end)
      return a.e->end < b.e->end;
    return a.idx < b.idx;
  });

  SmallVector<TextEdit, 32> norm;
  norm.reserve(ordered.size());

  for (size_t i = 0; i < ordered.size();) {
    const uint64_t s = ordered[i].e->start;
    const uint64_t t = ordered[i].e->end;

    size_t j = i + 1;
    while (j < ordered.size() && ordered[j].e->start == s &&
           ordered[j].e->end == t)
      ++j;

    if (j - i == 1) {
      norm.push_back(*ordered[i].e);
      i = j;
      continue;
    }

    // Multiple edits share the same byte span.
    if (s == t) {
      // INSERT at identical position: concatenate in order.
      TextEdit merged;
      merged.start = s;
      merged.end = t;

      size_t totalLen = 0;
      for (size_t k = i; k < j; ++k)
        totalLen += ordered[k].e->text.size();
      merged.text.reserve(totalLen);

      std::optional<uint64_t> mergedBByteBegin;
      std::optional<uint64_t> mergedBByteEnd;
      bool mergedBByteRangeContiguous = true;
      std::optional<uint64_t> mergedOutByteBegin;
      std::optional<uint64_t> mergedOutByteEnd;
      // Same-offset insertions become one physical edit in source order. The
      // sidecar can describe that merged edit only when the contributing
      // B-side witnesses tile one contiguous byte range in the same order.
      for (size_t k = i; k < j; ++k) {
        const uint64_t fragmentOutputBase =
            static_cast<uint64_t>(merged.text.size());
        merged.text.append(ordered[k].e->text);
        appendShiftedLineControlPruneCandidates(
            merged.lineControlPruneCandidates,
            ordered[k].e->lineControlPruneCandidates, fragmentOutputBase);
        appendShiftedLineControlSourceMappings(
            merged.lineControlSourceMappings,
            ordered[k].e->lineControlSourceMappings, fragmentOutputBase);
        if (ordered[k].e->pending)
          merged.pending = ordered[k].e->pending;
        merged.acceptedResults.insert(merged.acceptedResults.end(),
                                      ordered[k].e->acceptedResults.begin(),
                                      ordered[k].e->acceptedResults.end());
        if (auto bRange = TextEditMaterializedBByteRange(*ordered[k].e)) {
          if (!mergedBByteBegin) {
            mergedBByteBegin = bRange->first;
            mergedBByteEnd = bRange->second;
          } else if (mergedBByteEnd && *mergedBByteEnd == bRange->first) {
            mergedBByteEnd = bRange->second;
          } else {
            mergedBByteRangeContiguous = false;
          }
        } else if (materializedEditMappings) {
          mergedBByteRangeContiguous = false;
        }
        if (auto outRange =
                TextEditMaterializedOutputTextRange(*ordered[k].e)) {
          const uint64_t outBegin = fragmentOutputBase + outRange->first;
          const uint64_t outEnd = fragmentOutputBase + outRange->second;
          mergedOutByteBegin =
              mergedOutByteBegin ? std::min(*mergedOutByteBegin, outBegin)
                                 : outBegin;
          mergedOutByteEnd =
              mergedOutByteEnd ? std::max(*mergedOutByteEnd, outEnd) : outEnd;
        }
      }
      if (mergedBByteBegin && mergedBByteEnd && mergedBByteRangeContiguous)
        StampTextEditMaterializedBByteRange(merged, *mergedBByteBegin,
                                            *mergedBByteEnd);
      if (mergedOutByteBegin && mergedOutByteEnd)
        StampTextEditMaterializedOutputTextRange(merged, *mergedOutByteBegin,
                                                 *mergedOutByteEnd);

      deduplicateLineControlPruneCandidates(merged.lineControlPruneCandidates);
      deduplicateLineControlSourceMappings(merged.lineControlSourceMappings);

      norm.push_back(std::move(merged));
      i = j;
      continue;
    }

    // Non-zero span duplicates must be identical to compose safely. Choosing
    // an arbitrary writer for the same replaced byte range is not a proof; if
    // two producers disagree about the replacement payload, fail closed and let
    // the caller select the explicit terminal fallback path.
    const TextEdit *first = ordered[i].e;
    bool payloadsAgree = true;
    for (size_t k = i + 1; k < j; ++k) {
      if (ordered[k].e->text != first->text) {
        payloadsAgree = false;
        break;
      }
    }

    if (!payloadsAgree) {
      auto tryMergeDirectDuplicateFragments = [&]() -> std::optional<TextEdit> {
        SmallVector<const TextEdit *, 8> fragments;
        fragments.reserve(j - i);

        // This merge is only valid for duplicate fragments that still have a
        // direct one-to-one TU hunk witness. Anything synthesized, macro-owned,
        // include-owned, or missing hunk provenance must stay on the normal
        // composition path.
        for (size_t k = i; k < j; ++k) {
          const TextEdit &edit = *ordered[k].e;
          if (!edit.isDirectTUHunkEdit || !edit.directTUHunkIndex ||
              !edit.directTUHunkAStart || !edit.directTUHunkAEnd ||
              !edit.directTUHunkBStart || !edit.directTUHunkBEnd)
            return std::nullopt;
          fragments.push_back(&edit);
        }

        // Reconstruct the original hunk order before proving adjacency. The
        // duplicate group is ordered by emitted byte span, which is not enough
        // to prove that the B-side replacement interval is contiguous.
        sort(fragments, [](const TextEdit *lhs, const TextEdit *rhs) {
          return *lhs->directTUHunkIndex < *rhs->directTUHunkIndex;
        });

        // Adjacent duplicate fragments may be collapsed only when they form one
        // uninterrupted run in both the A token hunks and B token hunks. This
        // prevents merging unrelated fragments that merely overlap the same
        // emitted byte span.
        for (size_t k = 1; k < fragments.size(); ++k) {
          const TextEdit &prev = *fragments[k - 1];
          const TextEdit &cur = *fragments[k];

          if (*cur.directTUHunkIndex != *prev.directTUHunkIndex + 1)
            return std::nullopt;
          if (*cur.directTUHunkAStart != *prev.directTUHunkAEnd)
            return std::nullopt;
          if (*cur.directTUHunkBStart != *prev.directTUHunkBEnd)
            return std::nullopt;
        }

        // The merged replacement is exactly the contiguous B-token envelope
        // covered by the duplicate fragment run.
        const uint64_t bBegin = *fragments.front()->directTUHunkBStart;
        const uint64_t bEnd = *fragments.back()->directTUHunkBEnd;
        if (bBegin >= bEnd || bEnd > static_cast<uint64_t>(bToks_.size()))
          return std::nullopt;

        StringRef replacement =
            refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_, bBegin, bEnd);
        if (replacement.empty())
          return std::nullopt;

        // Apply normal resync handling to the whole duplicate byte span, rather
        // than preserving each fragment's already-conflicting local resync.
        ResyncOutcome ro = ApplyResyncOrPend(originalFileText, s, t,
                                             replacement, emissionOwner,
                                             ownerIncludeId);
        TextEdit merged{s, t, std::move(ro.text), std::move(ro.pending),
                        std::nullopt, {}, {}, {}};
        merged.lineControlPruneCandidates =
            std::move(ro.lineControlPruneCandidates);
        StampTextEditMaterializedBTokenRange(merged, bBegin, bEnd);

        // Preserve existing proof carriers from the fragments. If the fragments
        // carry macro-root provenance, they must all agree on the same root.
        for (const TextEdit *fragment : fragments) {
          if (fragment->expandedMacroRootId) {
            if (merged.expandedMacroRootId &&
                *merged.expandedMacroRootId != *fragment->expandedMacroRootId)
              return std::nullopt;
            merged.expandedMacroRootId = fragment->expandedMacroRootId;
          }
          merged.acceptedResults.insert(merged.acceptedResults.end(),
                                        fragment->acceptedResults.begin(),
                                        fragment->acceptedResults.end());
        }

        // Add a carrier for the actual emitted merged surface so the final
        // proof gate can reason about this replacement as one conservative TU
        // edit.
        AttachAcceptedResultCarrier(
            merged, proofLattice_.BuildAcceptedTUTextEditCandidate(
                        AcceptedPathKind::TUByteSpanConservativeEdit, s, t,
                        replacement));

        return merged;
      };

      // Split token-LCS frontiers can produce several direct TU hunk fragments
      // for the same original byte span. If those fragments are adjacent in
      // both A-token and B-token space, they are not competing writers: they
      // are one replacement that was fragmented before byte emission. Rebuild
      // that replacement from the closed B-token interval; otherwise keep the
      // existing fail-closed duplicate-edit policy.
      if (std::optional<TextEdit> mergedDirectDuplicate =
              tryMergeDirectDuplicateFragments()) {
        norm.push_back(std::move(*mergedDirectDuplicate));
        i = j;
        continue;
      }

      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/apply",
          llvm::formatv(
              "conflicting duplicate non-zero edits for span [{0},{1}) in {2}",
              s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }

    TextEdit merged = *first;
    std::optional<std::pair<uint64_t, uint64_t>> mergedBRange =
        TextEditMaterializedBByteRange(merged);
    bool mergedBRangeAmbiguous = false;
    std::optional<std::pair<uint64_t, uint64_t>> mergedOutRange =
        TextEditMaterializedOutputTextRange(merged);
    bool mergedOutRangeAmbiguous = false;
    for (size_t k = i + 1; k < j; ++k) {
      const TextEdit &dup = *ordered[k].e;

      // Identical non-zero edits collapse to one physical replacement, but any
      // deferred line-resync state produced by the later duplicate must still
      // be preserved on the merged edit.
      if (dup.pending)
        merged.pending = dup.pending;

      // If duplicate edits represent an expanded macro replacement, they must
      // agree on the same expanded root. Different roots for the same concrete
      // byte span would make the emitted edit's proof carrier ambiguous.
      if (merged.expandedMacroRootId && dup.expandedMacroRootId &&
          *merged.expandedMacroRootId != *dup.expandedMacroRootId) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::EmissionEditSetComposable,
                TerminalFallbackFailureReason::UncomposableEmissionEditSet),
            "edits/apply",
            llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                          "carry incompatible expanded macro roots {3} and {4}",
                          s, t, emissionOwner, *merged.expandedMacroRootId,
                          *dup.expandedMacroRootId)
                .str());
        return originalFileText.str();
      }

      // A duplicate may carry the expanded-root marker even when the first edit
      // did not. Preserve that marker so counter stabilization and audit still
      // see the emitted replacement as macro-expanded.
      if (!merged.expandedMacroRootId && dup.expandedMacroRootId)
        merged.expandedMacroRootId = dup.expandedMacroRootId;

      if (std::optional<std::pair<uint64_t, uint64_t>> dupBRange =
              TextEditMaterializedBByteRange(dup)) {
        if (!mergedBRange) {
          mergedBRange = dupBRange;
        } else if (mergedBRange->first != dupBRange->first ||
                   mergedBRange->second != dupBRange->second) {
          mergedBRangeAmbiguous = true;
        }
      }

      if (std::optional<std::pair<uint64_t, uint64_t>> dupOutRange =
              TextEditMaterializedOutputTextRange(dup)) {
        if (!mergedOutRange) {
          mergedOutRange = dupOutRange;
        } else if (mergedOutRange->first != dupOutRange->first ||
                   mergedOutRange->second != dupOutRange->second) {
          mergedOutRangeAmbiguous = true;
        }
      }

      // Keep all accepted-result carriers from the equivalent duplicates. The
      // text replacement is shared, but each proof witness still explains one
      // path that contributed to the emitted edit.
      appendShiftedLineControlPruneCandidates(
          merged.lineControlPruneCandidates, dup.lineControlPruneCandidates,
          /*delta=*/0);
      appendShiftedLineControlSourceMappings(
          merged.lineControlSourceMappings, dup.lineControlSourceMappings,
          /*delta=*/0);
      merged.acceptedResults.insert(merged.acceptedResults.end(),
                                    dup.acceptedResults.begin(),
                                    dup.acceptedResults.end());
    }

    if (mergedBRangeAmbiguous && materializedEditMappings) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          "edit-map",
          llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                        "carry different B-side materialization ranges",
                        s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }
    if (mergedOutRangeAmbiguous && materializedEditMappings) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          "edit-map",
          llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                        "carry different refolded-output materialization "
                        "ranges",
                        s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }
    if (mergedBRange)
      StampTextEditMaterializedBByteRange(merged, mergedBRange->first,
                                          mergedBRange->second);
    if (mergedOutRange)
      StampTextEditMaterializedOutputTextRange(merged, mergedOutRange->first,
                                               mergedOutRange->second);

    deduplicateLineControlPruneCandidates(merged.lineControlPruneCandidates);
    deduplicateLineControlSourceMappings(merged.lineControlSourceMappings);

    norm.push_back(std::move(merged));
    i = j;
  }

  auto sortNormalizedTextEdits = [](SmallVectorImpl<TextEdit> &items) {
    sort(items, [](const TextEdit &a, const TextEdit &b) {
      if (a.start != b.start)
        return a.start < b.start;
      return a.end < b.end;
    });
  };

  auto findDirectEditForHunk = [&](uint64_t hunkIndex)
      -> std::optional<size_t> {
    for (size_t idx = 0; idx < norm.size(); ++idx) {
      const TextEdit &edit = norm[idx];
      if (edit.directTUHunkIndex && *edit.directTUHunkIndex == hunkIndex)
        return idx;
    }
    return std::nullopt;
  };

  auto sourceTokenSpan = [&](uint64_t aTok)
      -> std::optional<std::pair<uint64_t, uint64_t>> {
    if (aTok >= static_cast<uint64_t>(aToks_.size()))
      return std::nullopt;
    if (auto span = tuEdits_.PlanTUByteSpan(aTok, aTok + 1, emissionOwner))
      return span->byteRange();
    return std::nullopt;
  };

  struct ClosedTURealizationCandidate {
    uint64_t firstHunk = 0;
    uint64_t lastHunk = 0;
    uint64_t sourceBegin = 0;
    uint64_t sourceEnd = 0;
    uint64_t bBegin = 0;
    uint64_t bEnd = 0;
    SmallVector<size_t, 16> editIndices;
  };

  // Validate that a contiguous run of direct TU-hunk edits can be replaced by
  // one closed TU realization. The candidate is accepted only when every hunk
  // in the run has a direct edit, the source/B envelopes are monotone and
  // non-empty, no independent edit intrudes into the source interval, and the
  // A↔B token map proves that all stable tokens crossing either envelope stay
  // inside the corresponding opposite envelope.
  auto checkClosedTURealizationCandidate =
      [&](uint64_t firstHunk, uint64_t lastHunk,
          ClosedTURealizationCandidate &candidate) -> bool {
    candidate = ClosedTURealizationCandidate{};
    candidate.firstHunk = firstHunk;
    candidate.lastHunk = lastHunk;

    // The closure is defined over a contiguous hunk run, so every hunk in the
    // run must already have a direct TU edit that can be absorbed.
    for (uint64_t hunkIndex = firstHunk; hunkIndex <= lastHunk; ++hunkIndex) {
      std::optional<size_t> editIndex = findDirectEditForHunk(hunkIndex);
      if (!editIndex)
        return false;
      candidate.editIndices.push_back(*editIndex);
      if (hunkIndex == std::numeric_limits<uint64_t>::max())
        break;
    }

    candidate.sourceBegin = std::numeric_limits<uint64_t>::max();
    candidate.sourceEnd = 0;
    candidate.bBegin = std::numeric_limits<uint64_t>::max();
    candidate.bEnd = 0;

    // Build the source byte envelope and B-token envelope for the selected
    // direct edits. The B intervals must appear in order; otherwise a single
    // replacement would not preserve the token-order proof for the hunk run.
    std::optional<uint64_t> previousBEnd;
    for (size_t editIndex : candidate.editIndices) {
      const TextEdit &edit = norm[editIndex];
      if (!edit.isDirectTUHunkEdit || !edit.directTUHunkBStart ||
          !edit.directTUHunkBEnd)
        return false;
      if (previousBEnd && *edit.directTUHunkBStart < *previousBEnd)
        return false;
      previousBEnd = edit.directTUHunkBEnd;
      candidate.sourceBegin = std::min(candidate.sourceBegin, edit.start);
      candidate.sourceEnd = std::max(candidate.sourceEnd, edit.end);
      candidate.bBegin = std::min(candidate.bBegin, *edit.directTUHunkBStart);
      candidate.bEnd = std::max(candidate.bEnd, *edit.directTUHunkBEnd);
    }

    // The synthesized replacement must cover a real source byte interval and a
    // real B-token interval.
    if (candidate.sourceBegin == std::numeric_limits<uint64_t>::max() ||
        candidate.sourceBegin >= candidate.sourceEnd)
      return false;
    if (candidate.bBegin == std::numeric_limits<uint64_t>::max() ||
        candidate.bBegin >= candidate.bEnd ||
        candidate.bEnd > static_cast<uint64_t>(bToks_.size()))
      return false;

    auto candidateContainsEdit = [&](size_t editIndex) {
      for (size_t candidateEditIndex : candidate.editIndices) {
        if (candidateEditIndex == editIndex)
          return true;
      }
      return false;
    };

    // The closure may compose only the selected hunk-run edits. Any other edit
    // that overlaps the source envelope, including a zero-width insertion
    // inside it, would need a separate composition law and is rejected here.
    for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
      if (candidateContainsEdit(editIndex))
        continue;
      const TextEdit &edit = norm[editIndex];
      const bool nonZeroOverlap =
          edit.start < candidate.sourceEnd && candidate.sourceBegin < edit.end;
      const bool interiorInsertion = edit.start == edit.end &&
                                     candidate.sourceBegin < edit.start &&
                                     edit.start < candidate.sourceEnd;
      if (nonZeroOverlap || interiorInsertion)
        return false;
    }

    // A-side closure: every A token whose source bytes overlap the candidate
    // must be wholly contained by the candidate source interval. If that token
    // has a stable B mate, that B mate must also lie inside the candidate
    // replacement envelope.
    for (uint64_t aTok = 0; aTok < static_cast<uint64_t>(aToks_.size());
         ++aTok) {
      std::optional<std::pair<uint64_t, uint64_t>> span = sourceTokenSpan(aTok);
      if (!span)
        continue;

      const bool overlapsSource = span->first < candidate.sourceEnd &&
                                  candidate.sourceBegin < span->second;
      if (!overlapsSource)
        continue;

      if (span->first < candidate.sourceBegin ||
          candidate.sourceEnd < span->second)
        return false;

      if (aTok < static_cast<uint64_t>(abTokMapA2B_.size())) {
        const int64_t mappedB = abTokMapA2B_[static_cast<size_t>(aTok)];
        if (mappedB >= 0 &&
            (static_cast<uint64_t>(mappedB) < candidate.bBegin ||
             static_cast<uint64_t>(mappedB) >= candidate.bEnd))
          return false;
      }
    }

    // B-side closure: every stable B token emitted by the replacement must map
    // back to an A token wholly contained by the source interval. This prevents
    // the synthesized edit from duplicating or moving stable outside tokens.
    for (uint64_t bTok = candidate.bBegin; bTok < candidate.bEnd; ++bTok) {
      if (bTok >= static_cast<uint64_t>(abTokMapB2A_.size()))
        return false;
      const int64_t mappedA = abTokMapB2A_[static_cast<size_t>(bTok)];
      if (mappedA < 0)
        continue;

      std::optional<std::pair<uint64_t, uint64_t>> span =
          sourceTokenSpan(static_cast<uint64_t>(mappedA));
      if (!span || span->first < candidate.sourceBegin ||
          candidate.sourceEnd < span->second)
        return false;
    }

    return true;
  };

  // Try to replace an uncomposable cluster of overlapping direct TU-hunk edits
  // with one conservative closed TU realization. The synthesized edit is
  // accepted only if a contiguous direct-hunk run can be proven closed in both
  // source-byte space and B-token space by the A↔B token maps.
  auto tryBuildClosedTURealization = [&](size_t overlapIndex,
                                         TextEdit &closure) -> bool {
    // Closed realization depends on both token-map directions: A tokens inside
    // the source interval must map into the B envelope, and stable B tokens in
    // the B envelope must map back into the source interval.
    if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
      return false;

    // Start from the overlap point and collect the full source-overlap cluster
    // around it. The cluster determines the minimum set of direct edits that
    // must be absorbed by the synthesized closure.
    size_t clusterBegin = overlapIndex == 0 ? 0 : overlapIndex - 1;
    uint64_t coverBegin = norm[clusterBegin].start;
    uint64_t coverEnd = norm[clusterBegin].end;
    while (clusterBegin > 0) {
      const TextEdit &prev = norm[clusterBegin - 1];
      if (prev.end <= coverBegin)
        break;
      --clusterBegin;
      coverBegin = std::min(coverBegin, prev.start);
      coverEnd = std::max(coverEnd, prev.end);
    }

    size_t clusterEnd = overlapIndex + 1;
    if (overlapIndex < norm.size()) {
      coverBegin = std::min(coverBegin, norm[overlapIndex].start);
      coverEnd = std::max(coverEnd, norm[overlapIndex].end);
    }
    while (clusterEnd < norm.size()) {
      const TextEdit &next = norm[clusterEnd];
      if (next.start >= coverEnd)
        break;
      coverBegin = std::min(coverBegin, next.start);
      coverEnd = std::max(coverEnd, next.end);
      ++clusterEnd;
    }

    // Every edit in the overlap cluster must be a direct TU-hunk edit. Mixed
    // edit classes would require a separate composition law, so fail closed.
    std::optional<uint64_t> hunkMin;
    std::optional<uint64_t> hunkMax;
    for (size_t i = clusterBegin; i < clusterEnd; ++i) {
      const TextEdit &edit = norm[i];
      if (!edit.isDirectTUHunkEdit || !edit.directTUHunkIndex)
        return false;
      hunkMin = hunkMin ? std::min(*hunkMin, *edit.directTUHunkIndex)
                        : *edit.directTUHunkIndex;
      hunkMax = hunkMax ? std::max(*hunkMax, *edit.directTUHunkIndex)
                        : *edit.directTUHunkIndex;
    }
    if (!hunkMin || !hunkMax)
      return false;

    // Expand to the maximal contiguous run of direct TU hunks available around
    // the cluster. The search below starts with the minimal cluster-covering
    // run and widens only as needed to find a closed realization.
    uint64_t directLo = *hunkMin;
    while (directLo > 0 && findDirectEditForHunk(directLo - 1))
      --directLo;

    uint64_t directHi = *hunkMax;
    while (directHi + 1 < static_cast<uint64_t>(abTokHunks_.size()) &&
           findDirectEditForHunk(directHi + 1))
      ++directHi;

    // Prefer the smallest closed hunk run that contains the overlap cluster.
    // This keeps the synthesized TU edit as local as possible while still
    // allowing widening when closure requires adjacent direct hunks.
    std::optional<ClosedTURealizationCandidate> best;
    const uint64_t minWidth = *hunkMax - *hunkMin + 1;
    const uint64_t maxWidth = directHi - directLo + 1;
    for (uint64_t width = minWidth; width <= maxWidth; ++width) {
      bool searchedAnyAtThisWidth = false;
      for (uint64_t first = directLo; first + width - 1 <= directHi; ++first) {
        const uint64_t last = first + width - 1;
        if (first > *hunkMin || last < *hunkMax)
          continue;
        searchedAnyAtThisWidth = true;

        ClosedTURealizationCandidate candidate;
        if (checkClosedTURealizationCandidate(first, last, candidate)) {
          best = std::move(candidate);
          break;
        }
      }
      if (best || !searchedAnyAtThisWidth)
        break;
    }
    if (!best)
      return false;

    // Materialize exactly the proven B-token envelope, then apply the same
    // line-resync machinery used by ordinary emitted edits.
    StringRef replacement = refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                                    best->bBegin, best->bEnd);
    ResyncOutcome ro =
        ApplyResyncOrPend(originalFileText, best->sourceBegin, best->sourceEnd,
                          replacement, emissionOwner, ownerIncludeId);
    closure =
        TextEdit{best->sourceBegin,     best->sourceEnd, std::move(ro.text),
                 std::move(ro.pending), std::nullopt,    {}, {}, {}};
    closure.lineControlPruneCandidates =
        std::move(ro.lineControlPruneCandidates);
    StampTextEditMaterializedBTokenRange(closure, best->bBegin, best->bEnd);
    AttachAcceptedResultCarrier(
        closure, proofLattice_.BuildAcceptedTUTextEditCandidate(
                     AcceptedPathKind::TUByteSpanConservativeEdit,
                     best->sourceBegin, best->sourceEnd, replacement));

    // Replace the absorbed direct edits with the single closed realization and
    // re-sort so downstream application sees a normal non-overlapping edit set.
    SmallVector<TextEdit, 32> resolved;
    resolved.reserve(norm.size() - best->editIndices.size() + 1);
    auto shouldRemove = [&](size_t editIndex) {
      for (size_t candidateEditIndex : best->editIndices) {
        if (candidateEditIndex == editIndex)
          return true;
      }
      return false;
    };
    for (size_t i = 0; i < norm.size(); ++i) {
      if (!shouldRemove(i))
        resolved.push_back(std::move(norm[i]));
    }
    resolved.push_back(std::move(closure));
    sortNormalizedTextEdits(resolved);
    norm = std::move(resolved);

    return true;
  };

  // Some token-LCS tie choices can split one logical B-side TU realization
  // around stable punctuation tokens. If the resulting direct TU hunk edits
  // overlap after lexical widening, first try to replace the minimal enclosing
  // direct-hunk run by one source/B-token-closed realization. This is not a
  // merge heuristic: it is accepted only when the A→B and B→A token maps prove
  // that the source interval and replacement interval are mutually closed.
  for (;;) {
    uint64_t cursor = 0;
    bool changed = false;
    for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
      const TextEdit &edit = norm[editIndex];
      if (edit.end < edit.start || edit.end > originalFileText.size())
        break;
      if (edit.start < cursor) {
        TextEdit closure;
        if (tryBuildClosedTURealization(editIndex, closure)) {
          changed = true;
          break;
        }
        changed = false;
        editIndex = norm.size();
        break;
      }
      cursor = edit.end;
    }
    if (!changed)
      break;
  }

  // The byte-edit emission boundary is proof-gated. By the time an edit
  // reaches this function, every non-terminal artifact it composes must
  // already carry normalized accepted-result carriers that are fully
  // discharged for emitted source text. Do not emit any edit whose carriers
  // are missing or not yet discharged.
  if (!AuditAcceptedEditProofs(norm, "emit/nonterminal", emissionOwner))
    return originalFileText.str();

  const size_t n = originalFileText.size();

  // Validate the normalized edit set before appending any bytes or recording
  // side effects. The applicator has only one sound composition law here:
  // edits must be in bounds and non-overlapping in original-file byte space.
  // If that law is violated, do not guess whether an insertion should be
  // merged into, ordered around, or shadowed by a replacement. Fail closed to
  // the explicit terminal fallback instead.
  uint64_t validatedCursor = 0;
  for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
    const TextEdit &e = norm[editIndex];
    if (e.end < e.start || e.end > n) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/apply",
          llvm::formatv("bad normalized edit bounds in {0}: edit#{1}=[{2},{3}) "
                        "fileLen={4} textLen={5}",
                        emissionOwner, editIndex, e.start, e.end, n,
                        e.text.size())
              .str());
      return originalFileText.str();
    }
    if (e.start < validatedCursor) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/apply",
          llvm::formatv("overlapping normalized edits in {0}: previousEnd={1} "
                        "edit#{2}=[{3},{4}) textLen={5}",
                        emissionOwner, validatedCursor, editIndex, e.start,
                        e.end, e.text.size())
              .str());
      return originalFileText.str();
    }
    validatedCursor = e.end;
  }

  // Assemble the final file by copying original slices between the normalized
  // edits and splicing each replacement at its staged byte range.
  SmallString<0> out;
  out.reserve(originalFileText.size() + 128);
  std::optional<PendingResync> pending = std::nullopt;

  uint64_t cursor = 0;

  for (const auto &e : norm) {
    // Copy the untouched source before this edit. If a previous replacement
    // could not safely emit its #line resync locally, this copy step is also the
    // next opportunity to flush that pending resync at a safe boundary.
    pending = AppendOriginalSliceWithPending(
        out, originalFileText, cursor, e.start, std::move(pending),
        sourceMappingOwner, ownerIncludeId, lineControlPruneCandidates,
        lineControlSourceMappings);

    // Record expanded macro roots that actually made it into the emitted text.
    // Counter stabilization uses this to force later __COUNTER__-sensitive
    // macro occurrences when needed.
    if (appliedExpandedMacroRootIds && e.expandedMacroRootId)
      appliedExpandedMacroRootIds->insert(*e.expandedMacroRootId);

    const uint64_t emittedBegin = static_cast<uint64_t>(out.size());
    out.append(e.text);
    if (lineControlPruneCandidates)
      appendShiftedLineControlPruneCandidates(*lineControlPruneCandidates,
                                              e.lineControlPruneCandidates,
                                              emittedBegin);
    if (lineControlSourceMappings)
      appendShiftedLineControlSourceMappings(*lineControlSourceMappings,
                                             e.lineControlSourceMappings,
                                             emittedBegin);

    // Record only the replacement bytes written by this edit. Copied original
    // slices and emitted #line/resync material are intentionally outside the
    // mapped source range because they are not materialized B edit payload.
    if (materializedEditMappings) {
      std::optional<std::pair<uint64_t, uint64_t>> bRange =
          TextEditMaterializedBByteRange(e);
      if (!bRange) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::EmissionArtifactDischarged,
        TerminalFallbackFailureReason::UndischargedEmissionArtifact), "edit-map",
            llvm::formatv("emitted edit in {0} at source=[{1},{2}) lacks a "
                          "deterministic B-side materialization range",
                          emissionOwner, e.start, e.end)
                .str());
        materializedEditMappings->clear();
        return originalFileText.str();
      }
      std::optional<std::pair<uint64_t, uint64_t>> outRange =
          TextEditMaterializedOutputTextRange(e);
      if (!outRange) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::EmissionArtifactDischarged,
                TerminalFallbackFailureReason::UndischargedEmissionArtifact),
            "edit-map",
            llvm::formatv("emitted edit in {0} at source=[{1},{2}) carries "
                          "invalid refolded-output materialization range",
                          emissionOwner, e.start, e.end)
                .str());
        materializedEditMappings->clear();
        return originalFileText.str();
      }
      materializedEditMappings->push_back(MaterializedEditMapping{
          bRange->first, bRange->second, emittedBegin + outRange->first,
          emittedBegin + outRange->second});
    }

    // Carry any resync that the edit itself could not emit locally. It will be
    // discharged by the next copied original slice, or by the final tail copy.
    if (e.pending) {
      pending = e.pending;
    }

    cursor = e.end;
  }

  // Copy the final untouched tail and flush any pending line resync that
  // survived the last replacement.
  pending = AppendOriginalSliceWithPending(
      out, originalFileText, cursor, n, std::move(pending), sourceMappingOwner,
      ownerIncludeId, lineControlPruneCandidates, lineControlSourceMappings);

  auto shiftLineControlMetadataAfterInsertion = [&](uint64_t pos,
                                                     uint64_t len) {
    if (len == 0)
      return;

    if (lineControlPruneCandidates) {
      for (FinalLineControlPruneCandidate &candidate :
           *lineControlPruneCandidates) {
        if (candidate.finalBegin >= pos) {
          candidate.finalBegin += len;
          candidate.finalEnd += len;
        } else if (candidate.finalEnd > pos) {
          // A synthetic directive should never straddle an independently
          // inserted repair.  Preserve a conservative range if it does rather
          // than creating overlapping stale coordinates.
          candidate.finalEnd += len;
        }
      }
    }

    if (lineControlSourceMappings) {
      std::vector<FinalLineControlSourceMapping> adjusted;
      adjusted.reserve(lineControlSourceMappings->size() + 1);
      for (FinalLineControlSourceMapping mapping : *lineControlSourceMappings) {
        if (mapping.finalEnd <= pos) {
          adjusted.push_back(std::move(mapping));
          continue;
        }
        if (mapping.finalBegin >= pos) {
          mapping.finalBegin += len;
          mapping.finalEnd += len;
          adjusted.push_back(std::move(mapping));
          continue;
        }

        // The insertion splits a copied source slice.  Keep precise provenance
        // for the surviving prefix and suffix; the synthetic #line bytes in the
        // middle intentionally receive no source mapping.
        const uint64_t sourceSplit =
            mapping.sourceBegin + (pos - mapping.finalBegin);
        FinalLineControlSourceMapping prefix = mapping;
        prefix.finalEnd = pos;
        prefix.sourceEnd = sourceSplit;
        if (prefix.finalBegin < prefix.finalEnd)
          adjusted.push_back(std::move(prefix));

        FinalLineControlSourceMapping suffix = mapping;
        suffix.finalBegin = pos + len;
        suffix.finalEnd += len;
        suffix.sourceBegin = sourceSplit;
        if (suffix.finalBegin < suffix.finalEnd)
          adjusted.push_back(std::move(suffix));
      }
      *lineControlSourceMappings = std::move(adjusted);
      deduplicateLineControlSourceMappings(*lineControlSourceMappings);
    }
  };

  auto sourceOffsetToFinalOffset =
      [&](uint64_t sourceOffset) -> std::optional<uint64_t> {
    if (!lineControlSourceMappings)
      return std::nullopt;
    for (const FinalLineControlSourceMapping &mapping :
         *lineControlSourceMappings) {
      if (mapping.ownerIncludeId != ownerIncludeId)
        continue;
      if (mapping.physicalFile != sourceMappingOwner)
        continue;
      if (sourceOffset < mapping.sourceBegin ||
          sourceOffset > mapping.sourceEnd)
        continue;
      if (sourceOffset == mapping.sourceEnd)
        return mapping.finalEnd;
      return mapping.finalBegin + (sourceOffset - mapping.sourceBegin);
    }
    return std::nullopt;
  };

  struct ConditionalJoinLineRepair {
    uint64_t sourceOffset = 0;
    uint64_t finalOffset = 0;
    std::string directive;
  };

  std::vector<ConditionalJoinLineRepair> joinRepairs;
  if (lineDirs_.Enabled() && lineControlSourceMappings &&
      !sourceMappingOwner.empty()) {
    for (const RefoldModel::CondGroup *group :
         model_.GetCondGroups(emissionOwner, ownerIncludeId)) {
      if (!group || group->file != emissionOwner ||
          group->parentIncludeId != ownerIncludeId)
        continue;

      bool groupContainsEdit = false;
      for (const TextEdit &edit : norm) {
        const bool zeroWidthInside =
            edit.start == edit.end && group->ContainsByte(edit.start);
        const bool overlap =
            edit.start < group->groupE && edit.end > group->groupB;
        if (zeroWidthInside || overlap) {
          groupContainsEdit = true;
          break;
        }
      }
      if (!groupContainsEdit)
        continue;

      std::optional<LineStateObserverSite> firstObserver =
          lineControlProof_.FirstOwnerSuffixLineStateObserverSite(ownerIncludeId, emissionOwner,
                                               group->groupE);
      if (!firstObserver || !firstObserver->demand.Any())
        continue;

      // If the first observer is still inside the conditional group, an
      // arm-local repair is the only statement that can dominate it.  The join
      // repair is required only for observers reached after the group has
      // rejoined.
      if (firstObserver->offset < group->groupE)
        continue;

      std::optional<uint64_t> finalOffset =
          sourceOffsetToFinalOffset(firstObserver->offset);
      const OwnerStateBoundary observerBoundary =
          OwnerStateBoundary::FromSource(OwnerSourceRange::From(
              emissionOwner, firstObserver->offset, firstObserver->offset,
              ownerIncludeId));

      auto forEachLineControlDemandComponent =
          [&](const LineStateObserverDemand &componentDemand, auto &&fn) {
            if (componentDemand.needsLine)
              fn(OwnerStateComponent::LineNumber);
            if (componentDemand.needsFile)
              fn(OwnerStateComponent::FileState);
            if (componentDemand.needsFileName)
              fn(OwnerStateComponent::FileName);
          };

      if (!finalOffset) {
        forEachLineControlDemandComponent(
            firstObserver->demand, [&](OwnerStateComponent component) {
              const std::string detail =
                  llvm::formatv("cannot map conditional join observer in {0}: "
                                "group#{1} observerSource={2}",
                                emissionOwner, group->id, firstObserver->offset)
                      .str();
              (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
                  observerBoundary, component, StateMutationKind::Replayed,
                  ownerStateProof_.BuildStateTransitionWitness(
                      SuffixStabilityWitnessKind::TerminalStateFailure,
                      component, observerBoundary, detail),
                  "linedir/conditional-join", detail,
                  /*requireKnownObserver=*/true);
            });
        return originalFileText.str();
      }

      LineDirectiveLocation loc =
          LineDirectiveInserter::LogicalLocationAtOffset(
              originalFileText, firstObserver->offset, emissionOwner, model_,
              emissionOwner, ownerIncludeId);

      // Conditional-join repairs must describe the logical line state that
      // reaches the first suffix observer after the conditional group rejoins.
      // The lexical scanner above can reconstruct ordinary source-local #line
      // state, but producer-backed recovery is needed for source-authored #line
      // operands whose evaluated state was recorded by the producer.

      if (!loc.producerProven) {
        if (std::optional<LineDirectiveLocation> producerLoc =
                lineControlProof_.ProducerBackedLineControlLocationAt(
                    originalFileText, emissionOwner, ownerIncludeId,
                    firstObserver->offset, firstObserver->offset)) {
          loc = std::move(*producerLoc);
        }
      }

      std::string directive =
          lineDirs_.FormatLineDirective(loc.lineNo, loc.fileSpelling);
      if (directive.empty())
        continue;

      auto finalLineBeginForOffset = [&](uint64_t offset) -> uint64_t {
        const uint64_t boundedOffset =
            std::min<uint64_t>(offset, static_cast<uint64_t>(out.size()));
        uint64_t lineBegin = boundedOffset;
        while (lineBegin > 0 && out[lineBegin - 1] != '\n')
          --lineBegin;
        return lineBegin;
      };

      auto immediatelyPrecededBySameLineDirective =
          [&](uint64_t insertionOffset, StringRef directiveText) -> bool {
        if (insertionOffset == 0 || directiveText.empty())
          return false;

        uint64_t prevLineEnd = insertionOffset;
        if (prevLineEnd > 0 && out[prevLineEnd - 1] == '\n')
          --prevLineEnd;
        uint64_t prevLineBegin = prevLineEnd;
        while (prevLineBegin > 0 && out[prevLineBegin - 1] != '\n')
          --prevLineBegin;

        StringRef prevLine(out.data() + prevLineBegin,
                           static_cast<size_t>(prevLineEnd - prevLineBegin));
        return prevLine.trim() == directiveText.trim();
      };

      const uint64_t insertionOffset = finalLineBeginForOffset(*finalOffset);
      StringRef currentOutAtInsertion(out.data(),
                                      static_cast<size_t>(insertionOffset));

      auto previousLineControlIsInsideThisConditionalGroup = [&]() -> bool {
        std::optional<std::pair<uint64_t, uint64_t>> previousDirective =
            findLastLineControlDirectiveRangeBefore(
                StringRef(out.data(), out.size()), insertionOffset);
        if (!previousDirective)
          return false;

        std::optional<uint64_t> groupFinalBegin =
            sourceOffsetToFinalOffset(group->groupB);
        std::optional<uint64_t> groupFinalEnd =
            sourceOffsetToFinalOffset(group->groupE);
        if (!groupFinalBegin || !groupFinalEnd)
          return false;

        return previousDirective->first >= *groupFinalBegin &&
               previousDirective->second <= *groupFinalEnd;
      };

      if (!LineDirectiveInserter::ShouldEmitLineDirective(
              currentOutAtInsertion, loc.fileSpelling, loc.lineNo, directive)) {
        if (!previousLineControlIsInsideThisConditionalGroup()) {
          forEachLineControlDemandComponent(
              firstObserver->demand, [&](OwnerStateComponent component) {
                const std::string detail =
                    llvm::formatv("suppress post-conditional #line in {0}: "
                                  "group#{1} observerSource={2}; current "
                                  "stream state is equivalent",
                                  emissionOwner, group->id,
                                  firstObserver->offset)
                        .str();
                (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
                    observerBoundary, component,
                    StateMutationKind::PreservedAcrossReplacement,
                    ownerStateProof_.BuildStateTransitionWitness(
                        SuffixStabilityWitnessKind::StateRepair, component,
                        observerBoundary, detail),
                    "linedir/conditional-join", detail,
                    /*requireKnownObserver=*/true);
              });
          continue;
        }
      }

      if (immediatelyPrecededBySameLineDirective(insertionOffset, directive)) {
        forEachLineControlDemandComponent(
            firstObserver->demand, [&](OwnerStateComponent component) {
              const std::string detail =
                  llvm::formatv("suppress duplicate post-conditional #line in "
                                "{0}: group#{1} observerSource={2}",
                                emissionOwner, group->id, firstObserver->offset)
                      .str();
              (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
                  observerBoundary, component,
                  StateMutationKind::PreservedAcrossReplacement,
                  ownerStateProof_.BuildStateTransitionWitness(
                      SuffixStabilityWitnessKind::StateRepair, component,
                      observerBoundary, detail),
                  "linedir/conditional-join", detail,
                  /*requireKnownObserver=*/true);
            });
        continue;
      }

      forEachLineControlDemandComponent(
          firstObserver->demand, [&](OwnerStateComponent component) {
            const std::string detail =
                llvm::formatv("post-conditional synthetic #line repair in {0}: "
                              "group#{1} observerSource={2} final={3}",
                              emissionOwner, group->id, firstObserver->offset,
                              insertionOffset)
                    .str();
            (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
                observerBoundary, component, StateMutationKind::Replayed,
                ownerStateProof_.BuildStateTransitionWitness(
                    SuffixStabilityWitnessKind::StateRepair, component,
                    observerBoundary, detail),
                "linedir/conditional-join", detail,
                /*requireKnownObserver=*/true);
          });

      joinRepairs.push_back(ConditionalJoinLineRepair{
          firstObserver->offset, insertionOffset, std::move(directive)});
    }
  }

  if (!joinRepairs.empty()) {
    llvm::sort(joinRepairs,
               [](const ConditionalJoinLineRepair &lhs,
                  const ConditionalJoinLineRepair &rhs) {
                 if (lhs.finalOffset != rhs.finalOffset)
                   return lhs.finalOffset > rhs.finalOffset;
                 if (lhs.sourceOffset != rhs.sourceOffset)
                   return lhs.sourceOffset > rhs.sourceOffset;
                 return lhs.directive < rhs.directive;
               });
    joinRepairs.erase(
        std::unique(joinRepairs.begin(), joinRepairs.end(),
                    [](const ConditionalJoinLineRepair &lhs,
                       const ConditionalJoinLineRepair &rhs) {
                      return lhs.finalOffset == rhs.finalOffset &&
                             lhs.directive == rhs.directive;
                    }),
        joinRepairs.end());

    for (const ConditionalJoinLineRepair &repair : joinRepairs) {
      out.insert(out.begin() + static_cast<size_t>(repair.finalOffset),
                 repair.directive.begin(), repair.directive.end());
      shiftLineControlMetadataAfterInsertion(
          repair.finalOffset, static_cast<uint64_t>(repair.directive.size()));
    }
  }

  if (lineControlPruneCandidates)
    deduplicateLineControlPruneCandidates(*lineControlPruneCandidates);
  if (lineControlSourceMappings)
    deduplicateLineControlSourceMappings(*lineControlSourceMappings);
  return std::string(out.str());
}

std::optional<RefoldTextEditAssembler::PendingResync>
RefoldTextEditAssembler::AppendOriginalSliceWithPending(
    SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
    uint64_t to, std::optional<RefoldTextEditAssembler::PendingResync> pending,
    StringRef emissionOwner, std::optional<uint64_t> ownerIncludeId,
    std::vector<FinalLineControlPruneCandidate>
        *lineControlPruneCandidates,
    std::vector<FinalLineControlSourceMapping>
        *lineControlSourceMappings) const {
  auto appendMappedOriginalSlice = [&](uint64_t begin, uint64_t end) {
    if (begin >= end)
      return;
    const uint64_t finalBegin = static_cast<uint64_t>(out.size());
    auto slice = original.slice(static_cast<size_t>(begin),
                                static_cast<size_t>(end));
    out.append(slice.begin(), slice.end());
    const uint64_t finalEnd = static_cast<uint64_t>(out.size());
    if (lineControlSourceMappings && !emissionOwner.empty()) {
      lineControlSourceMappings->push_back(FinalLineControlSourceMapping{
          finalBegin, finalEnd, emissionOwner.str(), begin, end,
          ownerIncludeId});
    }
  };

  // Fast path: if there is no pending #line correction, or line directives are
  // disabled, this helper is just a straight copy of the untouched source
  // slice.
  if (!pending || !lineDirs_.Enabled()) {
    appendMappedOriginalSlice(from, to);
    return std::nullopt;
  }

  if (pending->deferToConditionalJoin) {
    appendMappedOriginalSlice(from, to);
    return pending;
  }

  auto checkPendingLineControlRepair =
      [&](uint64_t sourceOffset, StateMutationKind mutation, StringRef detail) {
        const OwnerStateBoundary boundary = OwnerStateBoundary::FromSource(
            OwnerSourceRange::From(pending->fileSpellingForDir, sourceOffset,
                                   sourceOffset, pending->ownerIncludeId));
        return ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
            boundary, OwnerStateComponent::LineNumber, mutation,
            ownerStateProof_.BuildStateTransitionWitness(SuffixStabilityWitnessKind::StateRepair,
                                        OwnerStateComponent::LineNumber,
                                        boundary, detail),
            "line/pending", detail, /*requireKnownObserver=*/false);
      };

  uint64_t i = from;

  auto canFlushBeforeUntouchedDirectiveLine = [&](size_t pos) -> bool {
    if (pos >= original.size())
      return false;

    // Find the physical start of the current line and require that `pos` is
    // still inside that line's indentation prefix.
    size_t lineStart = pos;
    while (lineStart > 0 && original[lineStart - 1] != '\n')
      --lineStart;

    if (!stringutils::isIndentOnly(original, lineStart, pos))
      return false;

    size_t lineEnd = original.find('\n', pos);
    if (lineEnd == StringRef::npos)
      lineEnd = original.size();

    // A preprocessor directive may be preceded by horizontal whitespace. If the
    // first non-whitespace character is '#', flushing before the indentation is
    // still a directive-line-safe resync point.
    size_t firstNonWs = pos;
    while (firstNonWs < lineEnd && stringutils::isWs(original[firstNonWs]) &&
           original[firstNonWs] != '\n')
      ++firstNonWs;

    return firstNonWs < lineEnd && original[firstNonWs] == '#';
  };

  auto logicalLocationForPendingFlush =
      [&](uint64_t pos) -> std::optional<LineDirectiveLocation> {
    LineDirectiveLocation loc = LineDirectiveInserter::LogicalLocationAtOffset(
        original, static_cast<size_t>(pos), pending->fileSpellingForDir, model_,
        pending->fileSpellingForDir, pending->ownerIncludeId);
    if (loc.producerProven)
      return loc;

    // If the local scanner found an unmodeled source #line in the bytes copied
    // after the edit, the pending state can no longer be propagated by counting
    // newlines: that directive may have overwritten the logical stream state.
    if (loc.unprovenLineControlDirectiveOffset &&
        *loc.unprovenLineControlDirectiveOffset >= pending->resumeOffset &&
        *loc.unprovenLineControlDirectiveOffset < pos)
      return std::nullopt;

    // The pending object was created only after ApplyResyncOrPend proved the
    // logical state at resumeOffset.  If the eventual flush point is later in
    // untouched source, and no intervening unmodeled line-control directive was
    // found, advance that proved state by the physical non-spliced newlines
    // copied from the original between resumeOffset and pos.  This handles
    // source-authored #line directives whose operands used imported macro
    // state: the local scanner cannot re-evaluate them, but the producer
    // already did.
    if (pending->resumeLineNo == 0 || pos < pending->resumeOffset)
      return std::nullopt;

    const size_t delta = stringutils::countNonSplicedNewlines(
        original, pending->resumeOffset, pos);
    return LineDirectiveLocation(pending->resumeFileSpelling,
                                 pending->resumeLineNo + delta, true);
  };

  // Prefer to flush the pending resync before copying this slice when the
  // emitted output is already at BOL and the untouched input also resumes at a
  // safe line boundary. This handles both true BOL and the indentation prefix
  // of an untouched preprocessor directive line.
  if (stringutils::outAtBOL(StringRef(out.data(), out.size())) &&
      (stringutils::isBOL(original, static_cast<size_t>(from)) ||
       canFlushBeforeUntouchedDirectiveLine(static_cast<size_t>(from)))) {
    std::optional<LineDirectiveLocation> loc =
        logicalLocationForPendingFlush(from);
    if (!loc) {
      appendMappedOriginalSlice(from, to);
      return std::nullopt;
    }

    std::string directive =
        lineDirs_.FormatLineDirective(loc->lineNo, loc->fileSpelling);

    StringRef currentOut(out.data(), out.size());

    // Avoid emitting a redundant directive when the current output state
    // already represents the requested file/line location.
    if (LineDirectiveInserter::ShouldEmitLineDirective(
            currentOut, loc->fileSpelling, loc->lineNo, directive)) {
      (void)checkPendingLineControlRepair(
          from, StateMutationKind::MovedLater,
          llvm::formatv("flush pending synthetic #line at slice begin {0}",
                        from)
              .str());
      const uint64_t begin = static_cast<uint64_t>(out.size());
      out.append(directive.begin(), directive.end());
      const uint64_t end = static_cast<uint64_t>(out.size());
      if (lineControlPruneCandidates &&
          pending->finalLineControlPruneEligible) {
        lineControlPruneCandidates->push_back(
            makeSyntheticLineControlPruneCandidate(
                begin, end, FinalLineDirective::Origin::SyntheticNewlineResync,
                FinalLineControlOwnerKey(pending->fileSpellingForDir,
                                         pending->ownerIncludeId),
                FinalLineControlObligation::CosmeticSyntheticResync));
      }
    } else {
      (void)checkPendingLineControlRepair(
          from, StateMutationKind::PreservedAcrossReplacement,
          llvm::formatv("suppress pending #line at slice begin {0}; "
                        "current output state is equivalent",
                        from)
              .str());
    }

    appendMappedOriginalSlice(from, to);
    return std::nullopt;
  }

  // If the slice does not begin at a safe flush point, copy forward until the
  // first real newline boundary. Line-spliced newlines are not safe because
  // they do not end the logical source line.
  while (i < to) {
    size_t nl = original.find('\n', i);
    if (nl == llvm::StringRef::npos || nl >= static_cast<size_t>(to))
      break;

    appendMappedOriginalSlice(i, static_cast<uint64_t>(nl + 1));

    i = nl + 1;

    if (!stringutils::isLineSplice(original, nl)) {
      std::optional<LineDirectiveLocation> loc =
          logicalLocationForPendingFlush(i);
      if (!loc) {
        pending = std::nullopt;
        break;
      }

      std::string directive =
          lineDirs_.FormatLineDirective(loc->lineNo, loc->fileSpelling);

      StringRef currentOut(out.data(), out.size());
      if (LineDirectiveInserter::ShouldEmitLineDirective(
              currentOut, loc->fileSpelling, loc->lineNo, directive)) {
        (void)checkPendingLineControlRepair(
            i, StateMutationKind::MovedLater,
            llvm::formatv("flush pending synthetic #line at safe newline {0}",
                          i)
                .str());
        const uint64_t begin = static_cast<uint64_t>(out.size());
        out.append(directive.begin(), directive.end());
        const uint64_t end = static_cast<uint64_t>(out.size());
        if (lineControlPruneCandidates &&
            pending->finalLineControlPruneEligible) {
          lineControlPruneCandidates->push_back(
              makeSyntheticLineControlPruneCandidate(
                  begin, end,
                  FinalLineDirective::Origin::SyntheticNewlineResync,
                  FinalLineControlOwnerKey(pending->fileSpellingForDir,
                                           pending->ownerIncludeId),
                  FinalLineControlObligation::CosmeticSyntheticResync));
        }
      } else {
        (void)checkPendingLineControlRepair(
            i, StateMutationKind::PreservedAcrossReplacement,
            llvm::formatv("suppress pending #line at safe newline {0}; "
                          "current output state is equivalent",
                          i)
                .str());
      }

      pending = std::nullopt;
      break;
    }
  }

  // Copy any bytes after the flush point, or the whole slice tail if no safe
  // flush point was found. In the latter case, return the still-pending resync
  // so a later original slice can discharge it.
  if (i < to) {
    appendMappedOriginalSlice(i, to);
  }
  return pending;
}
} // namespace refold
} // namespace clang

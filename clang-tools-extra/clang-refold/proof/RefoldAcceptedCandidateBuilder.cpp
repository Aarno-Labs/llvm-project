//===--- RefoldAcceptedCandidateBuilder.cpp ---------------------*- C++ -*-===//
//
// Accepted-result candidate factory — implementation.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldAcceptedCandidateBuilder.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "edit/RefoldTUEditPlanner.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptedResultPredicates.h"
#include "proof/RefoldOwnerStateProof.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldTokenTextAnalysis.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldAcceptedCandidateBuilder::RefoldAcceptedCandidateBuilder(
    Dependencies deps)
    : deps_(std::move(deps)) {}

void RefoldAcceptedCandidateBuilder::CertifySelectedMacroPatchCandidate(
    MacroPatch &patch, const AcceptedResultCandidate &candidate,
    llvm::StringRef role) const {
  // records the accepted-result carrier at the selector boundary.
  // This is report-only/provenance-bearing for now: emission still recertifies
  // macro carriers onto the emitted-source discharge rule, but no selected
  // macro result is allowed to survive as only a raw MacroPatch.
  if (candidate.kind != AcceptedResultCandidateKind::MacroPatch) {
    deps_.theoremAudit.ReportNoLegacyAuditFinding(
        RefoldTheoremAudit::MakeLegacyAuditEvidence(
            LegacyPathKind::PathSpecificProofMirror, role,
            llvm::formatv(
                "selected macro patch bytes=[{0},{1}) was certified with "
                "non-macro AcceptedResultCandidate kind={2}",
                patch.invRange.begin, patch.invRange.end, candidate.kind)
                .str()));
    return;
  }

  deps_.theoremAudit.AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "CertifySelectedMacroPatchCandidate");
  patch.selectedAcceptedCandidate = candidate;
}

::clang::refold::MacroSelectionCandidate
RefoldAcceptedCandidateBuilder::BuildMacroSelectionCandidate(
    const MacroPatch &patch, bool allowNonTopLevelMacroSelectorFailure) const {
  MacroSelectionCandidate candidate;
  candidate.selectorCandidate = BuildAcceptedMacroCandidate(patch);

  // Build the emitted carrier through the emission-specific macro gate, but do
  // not use it for ranking.  If it is not theorem-normalized, leave the
  // optional empty so a selected macro patch cannot accidentally certify a
  // selector-only proof onto MacroPatch::selectedAcceptedCandidate.
  AcceptedResultCandidate emittedCandidate =
      RecertifyAcceptedMacroCandidateForEmission(patch,
                                                 candidate.selectorCandidate);
  if (deps_.acceptedResultRanker.IsSelectableAcceptedResultCandidate(
          emittedCandidate))
    candidate.emittedCandidate = std::move(emittedCandidate);

  candidate.selectorOnly =
      allowNonTopLevelMacroSelectorFailure &&
      !deps_.acceptedResultRanker.IsSelectableAcceptedResultCandidate(
          candidate.selectorCandidate) &&
      AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
          candidate.selectorCandidate);
  return candidate;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::BuildAcceptedMacroCandidate(
    const MacroPatch &patch) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::MacroPatch;
  candidate.proofSummary =
      deps_.macroPatchProofClassifier.ClassifyMacroPatchProof(patch);
  if (patch.proof.kind == MacroPatchProofKind::WholeCoverRealization &&
      patch.wholeCover.aLo <= patch.wholeCover.aHi &&
      patch.wholeCover.bAdjLo <= patch.wholeCover.bAdjHi) {
    deps_.ownerRealizationProofBuilder
        .AttachMixedOwnerTilingWitnessForTokenEnvelope(
            candidate.proofSummary, patch.wholeCover.aLo, patch.wholeCover.aHi,
            patch.wholeCover.bAdjLo, patch.wholeCover.bAdjHi);
  }
  candidate.begin = patch.invRange.begin;
  candidate.end = patch.invRange.end;
  if (patch.materialized.hasBTokenRange) {
    candidate.hasTargetBTokenRange = true;
    candidate.targetBTokStart = patch.materialized.bTokStart;
    candidate.targetBTokEnd = patch.materialized.bTokEnd;
  }

  const bool isOrdinaryMacroActualRepair =
      patch.proof.kind == MacroPatchProofKind::ArgsOnlyStandard ||
      patch.proof.kind == MacroPatchProofKind::ArgsOnlyPairedPureInsertion;
  const bool macroActualArityStable =
      patch.proof.proofRootMacroId != 0 &&
      patch.proof.proofRootMacroId == patch.macroId;
  if (isOrdinaryMacroActualRepair && patch.proof.wholeEnvelopeReplay &&
      patch.proof.wholeEnvelopeReplay->replayValidated &&
      patch.materialized.hasBTokenRange && macroActualArityStable) {
    candidate.hasMacroActualRepairWitness = true;
    candidate.macroActualWholeEnvelopeReplayValidated = true;
    candidate.macroActualDefinitionTapeReplayValidated =
        patch.proof.wholeEnvelopeReplay->definitionTapeReplayValidated;
    candidate.macroActualArityStable = true;
  }

  if (patch.proof.generatedCalleeReplay &&
      patch.proof.generatedCalleeReplay->calleeChainDeterministic &&
      patch.proof.generatedCalleeReplay->replacementReplayValidated &&
      patch.proof.generatedCalleeReplay->solvedActualsMappedToRoot &&
      patch.materialized.hasBTokenRange) {
    candidate.hasGeneratedCalleeReplayWitness = true;
    candidate.generatedCalleeReplayWitness = *patch.proof.generatedCalleeReplay;
  }

  if (patch.proof.variadicCommaReplay && patch.materialized.hasBTokenRange &&
      patch.proof.preservesInvocationStructure &&
      !candidate.hasGeneratedCalleeReplayWitness) {
    candidate.hasVariadicCommaWitness = true;
    candidate.variadicCommaWitness = *patch.proof.variadicCommaReplay;
  }

  std::optional<ZeroTokenBoundaryWitness> synthesizedZeroTokenBoundary;
  if (!patch.proof.zeroTokenBoundaryReplay &&
      patch.proof.kind == MacroPatchProofKind::ArgsOnlyPairedPureInsertion &&
      patch.materialized.hasBTokenRange &&
      patch.proof.preservesInvocationStructure) {
    ZeroTokenBoundaryWitness witness;
    witness.ownerId = patch.proof.proofRootMacroId
                          ? patch.proof.proofRootMacroId
                          : patch.macroId;
    witness.ownerKind = "macro";
    witness.hasSourceAnchor = true;
    witness.sourceAnchor = patch.invRange.begin;
    witness.hasBTokenRange = true;
    witness.bTokStart = patch.materialized.bTokStart;
    witness.bTokEnd = patch.materialized.bTokEnd;
    witness.producerProven = true;
    witness.ownerClosed = true;
    witness.layoutStable = true;
    witness.observersStable = true;
    witness.counterStable = true;
    witness.fromPairedInsertion = true;
    witness.boundarySignature =
        llvm::formatv("macro-paired-pure-insertion:owner={0}:source=[{1},{2}):"
                      "b=[{3},{4})",
                      witness.ownerId, patch.invRange.begin, patch.invRange.end,
                      witness.bTokStart, witness.bTokEnd)
            .str();
    synthesizedZeroTokenBoundary = std::move(witness);
  }

  const ZeroTokenBoundaryWitness *zeroTokenWitnessForCandidate =
      patch.proof.zeroTokenBoundaryReplay
          ? &*patch.proof.zeroTokenBoundaryReplay
          : (synthesizedZeroTokenBoundary ? &*synthesizedZeroTokenBoundary
                                          : nullptr);
  if (zeroTokenWitnessForCandidate && patch.materialized.hasBTokenRange &&
      patch.proof.preservesInvocationStructure &&
      !candidate.hasGeneratedCalleeReplayWitness) {
    candidate.hasZeroTokenBoundaryWitness = true;
    candidate.zeroTokenBoundaryWitness = *zeroTokenWitnessForCandidate;
  }

  // Expose direct stringification and token-paste producer semantics in the
  // common witness key.  This does not revalidate or reprioritize the patch; it
  // only summarizes producer facts for macro patches that have already survived
  // their family-specific proof path.
  if (const RefoldModel::MacroInvocation *inv =
          deps_.macroTopology.FindMacroInvocationById(patch.macroId)) {
    if (!inv->stringifySpans.empty() && patch.materialized.hasBTokenRange &&
        patch.proof.preservesInvocationStructure &&
        !candidate.hasGeneratedCalleeReplayWitness) {
      candidate.hasStringificationWitness = true;
      candidate.stringificationRootMacroId = patch.proof.proofRootMacroId
                                                 ? patch.proof.proofRootMacroId
                                                 : patch.macroId;
      candidate.stringificationSpanCount =
          static_cast<uint32_t>(inv->stringifySpans.size());

      SmallVector<uint32_t, 8> argIdxs;
      std::vector<RefoldModel::PPArgSpan> spans = inv->stringifySpans;
      llvm::sort(spans, [](const RefoldModel::PPArgSpan &lhs,
                           const RefoldModel::PPArgSpan &rhs) {
        if (lhs.argIdx != rhs.argIdx)
          return lhs.argIdx < rhs.argIdx;
        if (lhs.begin != rhs.begin)
          return lhs.begin < rhs.begin;
        return lhs.end < rhs.end;
      });

      std::string producerSig =
          llvm::formatv("root={0}:spans={1}",
                        candidate.stringificationRootMacroId, spans.size())
              .str();
      std::string payloadSig;
      bool payloadsCanonical = true;
      for (const RefoldModel::PPArgSpan &span : spans) {
        argIdxs.push_back(span.argIdx);
        producerSig += llvm::formatv(":arg={0}:[{1},{2})", span.argIdx,
                                     span.begin, span.end)
                           .str();
        StringRef literal =
            deps_.sourceMapper.SliceASource(span.begin, span.end).trim();
        std::optional<std::string> unstringified =
            deps_.argTextRecovery.UnstringifyLiteralToArgText(
                literal, /*allowTopLevelComma=*/true);
        std::optional<std::string> canonical =
            unstringified ? stringutils::canonicalizeStringifyInversePayload(
                                StringRef(*unstringified))
                          : std::nullopt;
        if (!unstringified || !canonical ||
            StringRef(*canonical).trim() != StringRef(*unstringified).trim()) {
          payloadsCanonical = false;
          payloadSig +=
              llvm::formatv(":arg={0}:payload=unknown", span.argIdx).str();
          continue;
        }
        payloadSig += llvm::formatv(":arg={0}:canon={1}", span.argIdx,
                                    RefoldWitnessTrace::FormatWitnessTraceHash(
                                        *canonical))
                          .str();
      }
      llvm::sort(argIdxs);
      argIdxs.erase(std::unique(argIdxs.begin(), argIdxs.end()), argIdxs.end());
      candidate.stringificationArgCount = static_cast<uint32_t>(argIdxs.size());
      candidate.stringificationWhitespaceNormalized = payloadsCanonical;
      candidate.stringificationEscapedSpellingStable = payloadsCanonical;
      candidate.stringificationProducerSignature = std::move(producerSig);
      candidate.stringificationCanonicalPayloadSignature =
          payloadSig.empty() ? std::string("empty") : std::move(payloadSig);
      if (!payloadsCanonical) {
        // If the producer-recorded stringify literal cannot be normalized into
        // the supported inverse domain, do not let claim a known
        // stringification equivalence dimension. The already accepted macro
        // proof remains intact; the common resolver simply keeps this
        // dimension unknown until a later proof can explain it.
        candidate.hasStringificationWitness = false;
      }
    }

    const bool pasteProofPresent =
        patch.proof.paste.has_value() ||
        patch.proof.kind == MacroPatchProofKind::ArgsOnlyPasteSingle ||
        patch.proof.kind == MacroPatchProofKind::ArgsOnlyPasteMulti ||
        patch.proof.kind == MacroPatchProofKind::ArgsOnlyPurePasteOnly ||
        patch.proof.kind == MacroPatchProofKind::PasteDerivedCalleeSelector;
    if ((!inv->pasteSpans.empty() || !inv->pasteTokens.empty()) &&
        patch.materialized.hasBTokenRange && pasteProofPresent &&
        !candidate.hasGeneratedCalleeReplayWitness) {
      candidate.hasTokenPasteWitness = true;
      candidate.tokenPasteRootMacroId = patch.proof.proofRootMacroId
                                            ? patch.proof.proofRootMacroId
                                            : patch.macroId;
      candidate.tokenPasteSpanCount =
          static_cast<uint32_t>(inv->pasteSpans.size());
      candidate.tokenPasteTokenCount =
          static_cast<uint32_t>(inv->pasteTokens.size());

      std::string producerSig =
          llvm::formatv("root={0}:spans={1}:tokens={2}",
                        candidate.tokenPasteRootMacroId, inv->pasteSpans.size(),
                        inv->pasteTokens.size())
              .str();
      std::string resultSig;

      std::vector<RefoldModel::PPArgSpan> pasteSpans = inv->pasteSpans;
      llvm::sort(pasteSpans, [](const RefoldModel::PPArgSpan &lhs,
                                const RefoldModel::PPArgSpan &rhs) {
        if (lhs.argIdx != rhs.argIdx)
          return lhs.argIdx < rhs.argIdx;
        if (lhs.begin != rhs.begin)
          return lhs.begin < rhs.begin;
        return lhs.end < rhs.end;
      });
      for (const RefoldModel::PPArgSpan &span : pasteSpans) {
        producerSig += llvm::formatv(":span_arg={0}:[{1},{2})", span.argIdx,
                                     span.begin, span.end)
                           .str();
      }

      for (const RefoldModel::PasteToken &token : inv->pasteTokens) {
        resultSig += llvm::formatv(":result={0}:parts={1}",
                                   RefoldWitnessTrace::FormatWitnessTraceHash(
                                       token.spelling),
                                   token.parts.size())
                         .str();
        candidate.tokenPastePartCount +=
            static_cast<uint32_t>(token.parts.size());
        for (const RefoldModel::PastePart &part : token.parts) {
          if (part.kind == RefoldModel::PastePartKind::Arg) {
            ++candidate.tokenPasteArgPartCount;
            if (part.byteBegin == 0)
              candidate.tokenPasteHasLeftProducer = true;
            if (part.byteEnd == token.spelling.size())
              candidate.tokenPasteHasRightProducer = true;
            producerSig +=
                llvm::formatv(":arg_part={0}:[{1},{2})",
                              part.argIndex
                                  ? *part.argIndex
                                  : std::numeric_limits<uint32_t>::max(),
                              part.byteBegin, part.byteEnd)
                    .str();
          } else {
            ++candidate.tokenPasteLiteralPartCount;
            producerSig += llvm::formatv(":lit_part=[{0},{1})", part.byteBegin,
                                         part.byteEnd)
                               .str();
          }
        }
      }

      if (!candidate.tokenPasteHasLeftProducer &&
          candidate.tokenPasteArgPartCount != 0) {
        candidate.tokenPasteHasLeftProducer = true;
      }
      if (!candidate.tokenPasteHasRightProducer &&
          candidate.tokenPasteArgPartCount > 1) {
        candidate.tokenPasteHasRightProducer = true;
      }

      if (candidate.hasTargetBTokenRange) {
        for (uint64_t tok = candidate.targetBTokStart;
             tok < candidate.targetBTokEnd && tok < deps_.bToks.size(); ++tok) {
          resultSig +=
              llvm::formatv(":bkind={0}:bspell={1}", deps_.bToks[tok].kind,
                            RefoldWitnessTrace::FormatWitnessTraceHash(
                                deps_.bToks[tok].spelling))
                  .str();
        }
      }

      candidate.tokenPasteResultValidated =
          patch.proof.paste &&
          (patch.proof.paste->requiresProducerPasteSpans ||
           patch.proof.paste->replayValidated);
      candidate.tokenPasteDiagnosticSafe = candidate.tokenPasteResultValidated;
      candidate.tokenPasteProducerSignature = std::move(producerSig);
      candidate.tokenPasteResultSignature =
          resultSig.empty() ? std::string("empty") : std::move(resultSig);
    }
  }

  if (patch.proof.counterState) {
    candidate.hasCounterStateWitness = true;
    candidate.counterStateWitness = *patch.proof.counterState;
  }

  // Ordinary macro-repair/replay families may be counter-stable even when
  // they do not carry a value-specific CounterStateWitness.  The proof is not
  // textual: it is the producer macro-invocation tree.  If the accepted repair
  // preserves one macro invocation, the original expansion subtree contains no
  // producer-recorded __COUNTER__ invocation, and the emitted source repair
  // does not introduce a raw __COUNTER__ spelling, then this tile has zero
  // counter consumption delta.  In that case all suffix counter observers are
  // vacuously preserved by this tile.  Do not use this path for explicit
  // counter materialization/literalization, which already installed a typed
  // counter witness above.
  auto macroProofKindCanBeCounterNeutral = [](MacroPatchProofKind kind) {
    switch (kind) {
    case MacroPatchProofKind::ArgsOnlyStandard:
    case MacroPatchProofKind::DagSubtreeRoot:
    case MacroPatchProofKind::CallChainSuffix:
    case MacroPatchProofKind::DirectCalleeSubstitution:
    case MacroPatchProofKind::PasteDerivedCalleeSelector:
    case MacroPatchProofKind::RecursiveTupleGeneratedCalleeReplay:
      return true;
    case MacroPatchProofKind::Unknown:
    case MacroPatchProofKind::ArgsOnlyPasteSingle:
    case MacroPatchProofKind::ArgsOnlyPasteMulti:
    case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
    case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    case MacroPatchProofKind::CounterLiteral:
    case MacroPatchProofKind::WholeCoverRealization:
      return false;
    }
    return false;
  };

  auto macroInvocationIsInSubtreeOf =
      [&](const RefoldModel::MacroInvocation &macro, uint64_t rootId) {
        uint64_t currentId = macro.id;
        SmallVector<uint64_t, 8> seen;
        while (true) {
          if (currentId == rootId)
            return true;
          if (std::find(seen.begin(), seen.end(), currentId) != seen.end())
            return false;
          seen.push_back(currentId);

          const RefoldModel::MacroInvocation *current =
              deps_.macroTopology.FindMacroInvocationById(currentId);
          if (!current || !current->callerMacroId)
            return false;
          currentId = *current->callerMacroId;
        }
      };

  auto macroSubtreeContainsCounterInvocation = [&](uint64_t rootId) {
    if (!deps_.macroTopology.FindMacroInvocationById(rootId))
      return true;
    for (const RefoldModel::MacroInvocation &macro :
         deps_.model.GetMacroInvocations()) {
      if (macro.name != "__COUNTER__")
        continue;
      if (macroInvocationIsInSubtreeOf(macro, rootId))
        return true;
    }
    return false;
  };

  const uint64_t counterNeutralRootId = patch.proof.proofRootMacroId
                                            ? patch.proof.proofRootMacroId
                                            : patch.macroId;
  if (!candidate.hasCounterStateWitness && counterNeutralRootId != 0 &&
      macroProofKindCanBeCounterNeutral(patch.proof.kind) &&
      patch.proof.preservesInvocationStructure &&
      !deps_.tokenText.RawIdentifierAppearsInText("__COUNTER__",
                                                  patch.replacement) &&
      !macroSubtreeContainsCounterInvocation(counterNeutralRootId)) {
    candidate.hasCounterStateWitness = true;
    candidate.counterStateWitness = CounterStateWitness{};
    candidate.counterStateWitness.counterOrderKnown = true;
    candidate.counterStateWitness.suffixStateStable = true;
    candidate.counterStateWitness.suffixUnobserved = true;
    candidate.counterStateWitness.suffixObserverSignature =
        llvm::formatv(
            "counter-neutral:path={0}:root={1}:target=[{2},{3}):"
            "producer_subtree=no-counter:replacement=no-raw-counter",
            patch.proof.kind, counterNeutralRootId,
            candidate.hasTargetBTokenRange ? candidate.targetBTokStart : 0,
            candidate.hasTargetBTokenRange ? candidate.targetBTokEnd : 0)
            .str();
  }

  // Preserve the proof root separately from the byte span so selector/audit
  // code can reason about macro ancestry without reclassifying the patch.
  if (patch.proof.proofRootMacroId) {
    candidate.hasRootMacroId = true;
    candidate.rootMacroId = patch.proof.proofRootMacroId;
  }

  candidate.hasPayloadPreview = true;
  candidate.payloadPreview =
      stringutils::showWsWithClip(patch.replacement, 120);
  deps_.ownerRealizationProofBuilder.AttachStandardWitnesses(candidate);
  deps_.theoremAudit.AuditMacroPatchProofForLegacyAuthority(
      patch, "BuildAcceptedMacroCandidate");
  deps_.witnessTrace.TraceWitnessEmitted(
      deps_.witnessResolver.BuildRefoldWitness(candidate,
                                               "BuildAcceptedMacroCandidate"));
  return candidate;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::RecertifyAcceptedMacroCandidateForEmission(
    const MacroPatch &patch, AcceptedResultCandidate candidate) const {
  // Recertify emitted preserving macro artifacts onto the emission-specific
  // discharge rule, removing the byte-edit boundary's selector-only
  // nested-macro exception. Selector competition still uses the stronger
  // top-level proof-root contract through BuildAcceptedMacroCandidate().
  if (candidate.kind == AcceptedResultCandidateKind::MacroPatch &&
      candidate.proofSummary.theoremClass ==
          TheoremProofClass::InvocationPreservingProof &&
      AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
          candidate)) {
    candidate.proofSummary.discharge =
        deps_.macroPatchProofClassifier
            .ValidateEmittedInvocationPreservingProof(patch);
    deps_.proofSummaryBuilder.FinalizeProofSummary(candidate.proofSummary);
    deps_.ownerRealizationProofBuilder.AttachStandardWitnesses(candidate);
  }

  return candidate;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::BuildAcceptedMacroEmissionCandidate(
    const MacroPatch &patch) const {
  return RecertifyAcceptedMacroCandidateForEmission(
      patch, BuildAcceptedMacroCandidate(patch));
}

bool RefoldAcceptedCandidateBuilder::FinalizeSelectedMacroPatchForEmission(
    MacroPatch &patch, StringRef role) const {
  // A patch may be merged or materialized by a path that never participated in
  // the final macro selector.  Before the owner/macro-id map is flattened into
  // emission buckets, refresh the canonical proof summary and force every
  // emission-bound patch through the same theorem-normalized carrier gate.
  deps_.macroPatchProofClassifier.SyncMacroPatchProofSummary(patch);

  if (patch.selectedAcceptedCandidate)
    return true;

  SmallVector<AcceptedResultCandidate, 1> candidates;
  candidates.push_back(BuildAcceptedMacroEmissionCandidate(patch));

  const std::optional<SelectedAcceptedResultCandidate> selected =
      deps_.acceptedResultRanker.SelectPreferredAcceptedResultCandidate(
          candidates);
  if (selected) {
    CertifySelectedMacroPatchCandidate(patch, selected->candidate, role);
    return true;
  }

  const bool rejected =
      deps_.theoremAudit.RejectMissingSelectedMacroPatchCarrier(
          patch, role,
          llvm::formatv(
              "macro patch bytes=[{0},{1}) was queued for emission but no "
              "theorem-normalized AcceptedResultCandidate could be selected",
              patch.invRange.begin, patch.invRange.end)
              .str());

  // Diagnostic-only audit may keep collecting evidence in non-strict runs, but
  // strict engine runs and strict no-legacy audit runs must not forward an
  // uncertified MacroPatch into the emitted edit buckets.
  return !rejected;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::BuildAcceptedEmittedMacroCandidate(
    const MacroPatch &patch) const {
  // Makes the byte-edit boundary consume only the carrier selected before
  // emission bucketing.  Rebuilding from MacroPatch here would recreate the
  // legacy proof-authority escape that the selectedAcceptedCandidate invariant
  // is meant to eliminate.
  if (!patch.selectedAcceptedCandidate) {
    deps_.theoremAudit.RejectMissingSelectedMacroPatchCarrier(
        patch, "BuildAcceptedEmittedMacroCandidate",
        llvm::formatv(
            "emitted macro patch bytes=[{0},{1}) reached emission without "
            "a selected AcceptedResultCandidate",
            patch.invRange.begin, patch.invRange.end)
            .str());
    return AcceptedResultCandidate{};
  }

  deps_.theoremAudit.AuditAcceptedResultCandidateForLegacyAuthority(
      *patch.selectedAcceptedCandidate,
      "BuildAcceptedEmittedMacroCandidate/selected-carrier");
  return *patch.selectedAcceptedCandidate;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::BuildAcceptedIncludeCandidate(
    AcceptedPathKind currentPath, const IncludePatch &patch,
    const IncludeAnchorWitness *includeAnchorWitness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::IncludePatch;

  // Include-preserving candidates carry include-anchor proof metadata only.
  // Include realization is not a parallel include-specific proof family;
  // materialized/inline include output uses
  // BuildAcceptedIncludeRealizationCandidate() so the generic
  // OwnerRealizationWitness is the theorem-facing closure proof.
  candidate.proofSummary =
      deps_.acceptancePathClassifier.BuildAcceptedPathProofSummary(
          currentPath, &patch, /*tuAnchorWitness=*/nullptr,
          includeAnchorWitness,
          /*terminalFallbackWitness=*/nullptr);
  deps_.ownerRealizationProofBuilder
      .AttachMixedOwnerTilingWitnessForTokenEnvelope(candidate.proofSummary,
                                                     patch.aStart, patch.aEnd,
                                                     patch.bStart, patch.bEnd);

  candidate.begin = patch.aStart;
  candidate.end = patch.aEnd;

  if (patch.include) {
    candidate.hasOwnerIncludeId = true;
    candidate.ownerIncludeId = patch.include->id;
  }

  // Preserve anchor byte information for selector diagnostics and deterministic
  // tie-breaking without requiring later code to reopen the witness object.
  if (includeAnchorWitness && includeAnchorWitness->hasAnchorByte) {
    candidate.hasAnchorByte = true;
    candidate.anchorByte = includeAnchorWitness->anchorByte;
  }

  // Include-preserving insertions at an empty A range are zero-token
  // boundary-gap witnesses.  The existing include-anchor proof already
  // established the source byte; this mirrors that proof into the common
  // witness/equivalence vocabulary.
  if (includeAnchorWitness && patch.aStart == patch.aEnd &&
      includeAnchorWitness->hasAnchorByte) {
    ZeroTokenBoundaryWitness zeroToken;
    zeroToken.ownerKind = "include";
    zeroToken.ownerId = patch.include ? patch.include->id : 0;
    zeroToken.hasPPGap = true;
    zeroToken.ppGap = patch.aStart;
    zeroToken.hasSourceAnchor = true;
    zeroToken.sourceAnchor = includeAnchorWitness->anchorByte;
    zeroToken.hasBTokenRange = true;
    zeroToken.bTokStart = patch.bStart;
    zeroToken.bTokEnd = patch.bEnd;
    zeroToken.producerProven = true;
    zeroToken.ownerClosed = true;
    zeroToken.layoutStable = true;
    zeroToken.observersStable = true;
    zeroToken.counterStable = true;
    zeroToken.fromIncludeBoundary = true;
    zeroToken.fromDirectiveLayoutGap =
        includeAnchorWitness->evidence ==
            IncludeAnchorEvidenceKind::SelectedConditionalBoundary ||
        includeAnchorWitness->evidence ==
            IncludeAnchorEvidenceKind::DeclBoundary;
    zeroToken.boundarySignature =
        llvm::formatv("include-anchor:evidence={0}:include={1}:pp_gap={2}:"
                      "byte={3}:b=[{4},{5}):cond={6}:{7}:child={8}:{9}:"
                      "neighbor={10}:{11}",
                      includeAnchorWitness->evidence, zeroToken.ownerId,
                      patch.aStart, includeAnchorWitness->anchorByte,
                      patch.bStart, patch.bEnd,
                      includeAnchorWitness->hasCondArmId ? 1 : 0,
                      includeAnchorWitness->condArmId,
                      includeAnchorWitness->hasChildIncludeId ? 1 : 0,
                      includeAnchorWitness->childIncludeId,
                      includeAnchorWitness->hasNeighborPP ? 1 : 0,
                      includeAnchorWitness->neighborPP)
            .str();

    candidate.hasZeroTokenBoundaryWitness = true;
    candidate.zeroTokenBoundaryWitness = std::move(zeroToken);
  }

  candidate.hasPayloadPreview = true;
  candidate.payloadPreview =
      stringutils::showWsWithClip(patch.insertBytes, 120);
  FinalizeAcceptedCandidate(candidate, "BuildAcceptedIncludeCandidate");
  return candidate;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::BuildAcceptedIncludeRealizationCandidate(
    AcceptedPathKind currentPath, const RefoldModel::IncludeItem &include,
    IncludeRealizationEvidenceKind evidenceKind,
    std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope) const {
  const OwnerRealizationResult ownerRealization =
      deps_.ownerRealizationProofBuilder.BuildIncludeOwnerRealization(
          include, currentPath, evidenceKind, bTokenEnvelope);
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::IncludePatch;
  candidate.proofSummary =
      deps_.ownerRealizationProofBuilder.BuildOwnerRealizationProofSummary(
          currentPath, ownerRealization);
  candidate.begin = include.siteB;
  candidate.end = include.siteE;
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview = formatv("{0}", currentPath).str();

  if (bTokenEnvelope) {
    deps_.ownerRealizationProofBuilder
        .AttachMixedOwnerTilingWitnessForTokenEnvelope(
            candidate.proofSummary, include.cover.begin, include.cover.end,
            bTokenEnvelope->first, bTokenEnvelope->second);
  }

  candidate.hasOwnerIncludeId = true;
  candidate.ownerIncludeId = include.id;
  FinalizeAcceptedCandidate(candidate, "BuildAcceptedIncludeRealizationCandidate");
  return candidate;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::BuildAcceptedTUTextEditCandidate(
    AcceptedPathKind currentPath, const diffutils::Hunk &hunk,
    const TUByteSpanPlan &spanPlan,
    const StructuralHunkSegmentBinding *structuralBinding,
    StringRef payloadPreview) const {
  const OwnerRealizationResult ownerRealization =
      deps_.ownerRealizationProofBuilder.BuildTUOwnerRealization(
          currentPath, hunk, spanPlan, structuralBinding);
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TUTextEdit;
  candidate.proofSummary =
      deps_.ownerRealizationProofBuilder.BuildOwnerRealizationProofSummary(
          currentPath, ownerRealization);
  candidate.begin = spanPlan.tuByteBegin;
  candidate.end = spanPlan.tuByteEnd;
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview = payloadPreview.str();
  FinalizeAcceptedCandidate(candidate, "BuildAcceptedTUTextEditCandidate");
  return candidate;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::BuildAcceptedSpecializedTUTextEditCandidate(
    AcceptedPathKind currentPath, uint64_t begin, uint64_t end,
    StringRef payloadPreview) const {
  const OwnerRealizationResult ownerRealization =
      deps_.ownerRealizationProofBuilder.BuildSpecializedTUOwnerRealization(
          currentPath, begin, end);
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TUTextEdit;
  candidate.proofSummary =
      deps_.ownerRealizationProofBuilder.BuildOwnerRealizationProofSummary(
          currentPath, ownerRealization);
  candidate.begin = begin;
  candidate.end = end;
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview = payloadPreview.str();
  FinalizeAcceptedCandidate(candidate, "BuildAcceptedSpecializedTUTextEditCandidate");
  return candidate;
}

::clang::refold::AcceptedResultCandidate
RefoldAcceptedCandidateBuilder::BuildAcceptedTerminalCandidate(
    const TerminalFallbackWitness &witness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TerminalOutOfDomain;

  // Terminal fallback is intentionally represented as an accepted candidate
  // only after the proof inventory has declared the case out-of-domain.
  candidate.proofSummary =
      deps_.acceptancePathClassifier.BuildAcceptedPathProofSummary(
          AcceptedPathKind::TerminalEmitEditedPreprocessedStream,
          /*patch=*/nullptr, /*tuAnchorWitness=*/nullptr,
          /*includeAnchorWitness=*/nullptr, &witness);
  FinalizeAcceptedCandidate(candidate, "BuildAcceptedTerminalCandidate");
  return candidate;
}

void RefoldAcceptedCandidateBuilder::FinalizeAcceptedCandidate(
    AcceptedResultCandidate &candidate, StringRef builderName) const {
  deps_.ownerRealizationProofBuilder.AttachStandardWitnesses(candidate);
  deps_.theoremAudit.AuditAcceptedResultCandidateForLegacyAuthority(candidate,
                                                                   builderName);
  deps_.witnessTrace.TraceWitnessEmitted(
      deps_.witnessResolver.BuildRefoldWitness(candidate, builderName));
}

} // namespace refold
} // namespace clang

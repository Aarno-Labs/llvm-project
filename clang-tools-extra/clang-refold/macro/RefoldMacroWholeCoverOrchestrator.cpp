//===--- RefoldMacroWholeCoverOrchestrator.cpp ---------------*- C++ -*-===//
//
// Whole-cover macro patch orchestrator for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroWholeCoverOrchestrator.h"
#include "edit/RefoldBInsertionLedger.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

// Small token-spelling / PP-span helpers shared across the DAG phase
// services live in `macro/RefoldMacroDAGSharedHelpers.h`
// (`tokenSpellingVectorsEqual`, `hunkWithinPPSpans`,
// `ppSpanLessByTokenRange`, `ppArgSpanProofWidth`).  Include that header
// if you need them here.

// Build the orchestrator's owned phase services from the planner's public
// dependency bundle (`RefoldMacroPatchPlanner::Deps()`) plus a small set
// of std::function callbacks that forward into planner-side helpers.  Each
// phase is constructed once at orchestrator construction so the callbacks
// are not reconstructed on every
// `BuildMacroInvocationPatchWholeCover` call.
RefoldMacroWholeCoverOrchestrator::RefoldMacroWholeCoverOrchestrator(
    const RefoldMacroPatchPlanner *planner)
    : planner_(planner),
      argsOnlyPhase_(RefoldMacroArgsOnlyWholeCoverPhase::Dependencies{
          *planner_->Deps().sourceMapper, planner_->Deps().aToks,
          planner_->Deps().bToks, *planner_->Deps().abTokHunks,
          *planner_->Deps().proofLattice,
          [planner](const RefoldModel::MacroInvocation &m,
                    const diffutils::Hunk &h, llvm::StringRef baseInvText) {
            return planner->BuildMacroInvocationPatchArgsOnly(m, h,
                                                              baseInvText);
          }}),
      dagLeafDiscoveryPhase_(RefoldMacroDAGLeafDiscoveryPhase::Dependencies{
          *planner_->Deps().model, *planner_->Deps().macroTopology,
          *planner_->Deps().sourceMapper, *planner_->Deps().argTextRecovery,
          planner_->Deps().aToks, planner_->Deps().bToks,
          *planner_->Deps().abTokHunks, planner_->SubtreeReplayValidator(),
          [planner](const RefoldModel::MacroInvocation &m,
                    llvm::StringRef invText)
              -> std::optional<std::vector<std::pair<size_t, size_t>>> {
            return planner->GetMacroInvocationFormalArgContentRanges(m,
                                                                     invText);
          },
          [planner](const RefoldModel::MacroInvocation &m,
                    const diffutils::Hunk &h, llvm::StringRef baseInvText) {
            return planner->BuildMacroInvocationPatchArgsOnly(m, h,
                                                              baseInvText);
          }}),
      dagLiftingPhase_(RefoldMacroDAGLiftingPhase::Dependencies{
          *planner_->Deps().model, *planner_->Deps().sourceMapper,
          *planner_->Deps().lexLang, *planner_->Deps().lineDirs,
          *planner_->Deps().argTextRecovery, *planner_->Deps().macroTopology,
          planner_->Deps().aToks, planner_->Deps().bToks,
          *planner_->Deps().proofLattice,
          [planner](const RefoldModel::MacroInvocation &m,
                    llvm::StringRef invText)
              -> std::optional<std::vector<std::pair<size_t, size_t>>> {
            return planner->GetMacroInvocationFormalArgContentRanges(m,
                                                                     invText);
          },
          [planner](const RefoldModel::MacroInvocation &m, uint32_t argIdx,
                    llvm::StringRef baseArg, llvm::StringRef newArg,
                    llvm::ArrayRef<diffutils::Hunk> tokenHunks) {
            return planner
                ->MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
                    m, argIdx, baseArg, newArg, tokenHunks);
          },
          [this](const RefoldModel::MacroInvocation &m)
              -> std::optional<WholeCoverPlan> {
            return this->ComputeWholeCoverPlan(m);
          }}),
      patchReusePhase_(RefoldMacroPatchReusePhase::Dependencies{
          *planner_->Deps().lexLang,
          [planner](const RefoldModel::MacroInvocation &m,
                    llvm::StringRef invText)
              -> std::optional<std::vector<std::pair<size_t, size_t>>> {
            return planner->GetMacroInvocationFormalArgContentRanges(m,
                                                                     invText);
          }}),
      selectorSubstitutionPhase_(
          RefoldMacroSelectorSubstitutionPhase::Dependencies{
              *planner_->Deps().model, *planner_->Deps().sourceMapper,
              planner_->Deps().aToks, *planner_->Deps().proofLattice,
              [planner](const RefoldModel::MacroInvocation &m,
                        llvm::StringRef invText)
                  -> std::optional<std::vector<std::pair<size_t, size_t>>> {
                return planner->GetMacroInvocationFormalArgContentRanges(
                    m, invText);
              },
              [planner](llvm::ArrayRef<std::string> spellings, uint64_t bTok,
                        uint64_t eTok) {
                return planner->TokenSpellingsEqualToA(spellings, bTok, eTok);
              },
              [planner](llvm::ArrayRef<std::string> spellings, uint64_t bTok,
                        uint64_t eTok) {
                return planner->TokenSpellingsEqualToB(spellings, bTok, eTok);
              },
              [this](const RefoldModel::MacroInvocation &m,
                     llvm::StringRef baseText, llvm::StringRef newText) {
                return this->patchReusePhase_
                    .ValidateMergedDirectAndDagRootReplacement(m, baseText,
                                                               newText);
              }}),
      finalCandidateSelector_(RefoldMacroFinalCandidateSelector::Dependencies{
          *planner_->Deps().model, *planner_->Deps().macroTopology,
          *planner_->Deps().proofLattice, planner_->ReplayStabilityValidator(),
          planner_->ProofCertifier(), patchReusePhase_,
          [this](const RefoldModel::MacroInvocation &m)
              -> std::optional<WholeCoverPlan> {
            return this->ComputeWholeCoverPlan(m);
          },
          [this](const MacroPatch &patch, const WholeCoverPlan &plan,
                 uint64_t rootMacroId) {
            return this->WholeCoverPatchMatchesPlan(patch, plan, rootMacroId);
          },
          [planner](const MacroPatch &patch, const Owner &owner) {
            return planner->MacroPatchOwnerMatches(patch, owner);
          },
          [planner](llvm::StringRef arg) {
            return planner->IsParenthesizedTuple(arg);
          },
          [planner](const RefoldModel::MacroInvocation &m,
                    llvm::StringRef invText)
              -> std::optional<std::vector<std::pair<size_t, size_t>>> {
            return planner->GetMacroInvocationFormalArgContentRanges(m,
                                                                     invText);
          }}) {}

std::optional<WholeCoverPlan>
RefoldMacroWholeCoverOrchestrator::ComputeWholeCoverPlan(
    const RefoldModel::MacroInvocation &m) const {
  auto range = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
  if (!range)
    return std::nullopt;

  WholeCoverPlan plan;
  plan.covLoA = range->first;
  plan.covHiA = range->second;

  // Function-like macros with no formal parameters can have a useful body-span
  // range that is narrower than the producer's invocation cover. Record that
  // distinction so diagnostics can explain why the whole-cover domain came from
  // body material rather than the raw cover.
  plan.usedBodyRange =
      (m.subkind == "func" && m.defParams.empty() && !m.bodySpans.empty() &&
       (plan.covLoA != m.cover.begin || plan.covHiA != m.cover.end));

  // Whole-cover replay is admissible only when the selected A range is
  // explained by this invocation's own detailed macro provenance. Descendant
  // spans may justify a structure-preserving DAG lift, but coarse containment
  // alone does not prove that this callsite can be replaced by realized
  // whole-cover output.
  plan.selfContained =
      RefoldMacroWholeCoverProof::MacroWholeCoverIsSelfContained(m);
  if (!plan.selfContained) {
    return std::nullopt;
  }

  // Map the accepted A-token cover into B while preserving boundary insertions.
  // This gives the raw B envelope that the whole-cover candidate will replay.
  auto bEnv = (*planner_->Deps().sourceMapper)
                  .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                      plan.covLoA, plan.covHiA);
  if (!bEnv)
    return std::nullopt;

  plan.rawBTokStart = bEnv->first;
  plan.rawBTokEnd = bEnv->second;
  plan.bTokStart = plan.rawBTokStart;
  plan.bTokEnd = plan.rawBTokEnd;
  if (plan.bTokEnd <= plan.bTokStart)
    return std::nullopt;

  // If the mapped B envelope starts one token too far to the right, pull it
  // left when the immediately preceding B token matches the first A cover
  // token. This repairs boundary-placement drift without choosing by lexical
  // neighbor preference: the token must exactly be the cover boundary token.
  if (plan.covLoA < planner_->Deps().aToks.size() &&
      plan.bTokStart < planner_->Deps().bToks.size()) {
    StringRef want =
        planner_->Deps().aToks[static_cast<size_t>(plan.covLoA)].spelling;
    if (!want.empty()) {
      if (planner_->Deps().bToks[plan.bTokStart].spelling != want &&
          plan.bTokStart > 0 &&
          planner_->Deps().bToks[plan.bTokStart - 1].spelling == want) {
        plan.bTokStart--;
        plan.adjustedLeft = true;
      }
    }
  }

  // Symmetrically, if the mapped B envelope includes one token too far to the
  // right, contract it when the previous B token matches the last A cover
  // token. This keeps the replacement envelope aligned with the macro-owned
  // cover.
  if (plan.covHiA > 0 && (plan.covHiA - 1) < planner_->Deps().aToks.size() &&
      plan.bTokEnd > 0 && (plan.bTokEnd - 1) < planner_->Deps().bToks.size()) {
    StringRef want =
        planner_->Deps().aToks[static_cast<size_t>(plan.covHiA - 1)].spelling;
    if (!want.empty()) {
      // Do not contract a whole-cover replacement across a trailing B comment.
      // Comments are source trivia, not macro-body delimiter tokens; clipping
      // one out here splits an otherwise line-local insertion and forces a
      // spurious #line resynchronization before the comment text.
      const bool rightIsComment =
          planner_->Deps().bToks[plan.bTokEnd - 1].kind == "comment";
      if (!rightIsComment &&
          planner_->Deps().bToks[plan.bTokEnd - 1].spelling != want &&
          plan.bTokEnd >= 2 &&
          planner_->Deps().bToks[plan.bTokEnd - 2].spelling == want) {
        plan.bTokEnd--;
        plan.adjustedRight = true;
      }
    }
  }

  if (plan.bTokEnd <= plan.bTokStart)
    return std::nullopt;

  // Clip away B text that is already claimed by stronger/narrower accepted
  // material before using the whole-cover text. The raw vs. clipped comparison
  // records whether this candidate had to yield to existing claims.
  std::string unclipped = (*planner_->Deps().sourceMapper)
                              .SliceBSource(plan.bTokStart, plan.bTokEnd)
                              .str();
  std::string clipped =
      planner_->Deps().bInsertionLedger->SliceBSourceClippedAgainstClaims(
          plan.bTokStart, plan.bTokEnd);
  plan.claimsClipped = (unclipped != clipped);
  plan.clippedText = StringRef(clipped).trim().str();

  return plan;
}

bool RefoldMacroWholeCoverOrchestrator::WholeCoverPatchMatchesPlan(
    const MacroPatch &patch, const WholeCoverPlan &plan,
    uint64_t rootMacroId) const {
  if (patch.proof.kind != MacroPatchProofKind::WholeCoverRealization ||
      patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != rootMacroId)
    return false;
  return patch.wholeCover.usedBodyRange == plan.usedBodyRange &&
         patch.wholeCover.selfContained == plan.selfContained &&
         patch.wholeCover.adjustedLeft == plan.adjustedLeft &&
         patch.wholeCover.adjustedRight == plan.adjustedRight &&
         patch.wholeCover.claimsClipped == plan.claimsClipped &&
         patch.wholeCover.aLo == plan.covLoA &&
         patch.wholeCover.aHi == plan.covHiA &&
         patch.wholeCover.bRawLo == plan.rawBTokStart &&
         patch.wholeCover.bRawHi == plan.rawBTokEnd &&
         patch.wholeCover.bAdjLo == plan.bTokStart &&
         patch.wholeCover.bAdjHi == plan.bTokEnd;
}

// Whole-cover realization is admitted only by
// RefoldMacroWholeCoverProof::MacroWholeCoverIsSelfContained(), which proves
// the selected A-token cover directly from the invocation's own detailed
// provenance spans before certifying the shared OwnerRealizationProof. A nested
// child whose cover needs caller/descendant aggregation must instead be handled
// by structure-preserving DAG lifting or by a wider owner realization; coarse
// descendant span aggregation is not a separate proof class.
std::optional<MacroPatch>
RefoldMacroWholeCoverOrchestrator::TryCounterLiteralWholeCoverPatch(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    uint64_t invocationStart, uint64_t invocationEnd) const {
  // The only robust representation of an edited (or forced) __COUNTER__
  // expansion is to replace the invocation spelling with the B-side literal
  // token(s) for this specific occurrence. Do NOT whole-cover expand using the
  // macro cover, which may span multiple occurrences when a header is included
  // multiple times.
  if (m.name != "__COUNTER__")
    return std::nullopt;

  std::optional<std::string> repl;
  std::optional<std::pair<uint64_t, uint64_t>> counterBTokenRange;
  if (h.bEnd > h.bStart) {
    counterBTokenRange = std::make_pair(static_cast<uint64_t>(h.bStart),
                                        static_cast<uint64_t>(h.bEnd));
    repl = (*planner_->Deps().sourceMapper)
               .SliceBSource(static_cast<size_t>(h.bStart),
                             static_cast<size_t>(h.bEnd))
               .trim()
               .str();
  } else {
    auto bEnv = (*planner_->Deps().sourceMapper)
                    .MapATokRangeAToBTokenEnvelope(h.aStart, h.aEnd);
    if (bEnv && bEnv->second > bEnv->first) {
      counterBTokenRange = std::make_pair(static_cast<uint64_t>(bEnv->first),
                                          static_cast<uint64_t>(bEnv->second));
      repl = (*planner_->Deps().sourceMapper)
                 .SliceBSource(bEnv->first, bEnv->second)
                 .trim()
                 .str();
    }
  }
  if (!repl)
    return std::nullopt;

  CounterEventIdentity event =
      (*planner_->Deps().macroTopology)
          .BuildCounterEventIdentity(m, /*occurrenceOrdinal=*/0, h.aStart,
                                     h.aEnd, m.ownerIncludeId);
  event.expectedBValue = *repl;
  const OwnerStateBoundary boundary =
      planner_->GetOwnerStateProof().CounterStateBoundaryForEvent(event);
  const std::string detail =
      llvm::formatv("literalized direct __COUNTER__ invocation #{0} "
                    "A=[{1},{2})",
                    m.id, h.aStart, h.aEnd)
          .str();
  SuffixStabilityWitness counterWitness =
      planner_->GetOwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::Literalization,
          OwnerStateComponent::Counter, boundary,
          llvm::formatv(
              "{0}; {1}", detail,
              planner_->GetOwnerStateProof().FormatCounterEventForWitness(
                  event))
              .str());
  (void)planner_->GetOwnerStateProof().CheckStateTransitionAcrossEditBoundary(
      boundary, OwnerStateComponent::Counter, StateMutationKind::Literalized,
      counterWitness, "counter", detail, /*requireKnownObserver=*/false);

  MacroPatch patch{invocationStart, invocationEnd, std::move(*repl), m.id};
  if (counterBTokenRange &&
      counterBTokenRange->first <= counterBTokenRange->second &&
      counterBTokenRange->second <= planner_->Deps().bToks.size()) {
    patch.materialized.hasBTokenRange = true;
    patch.materialized.bTokStart = counterBTokenRange->first;
    patch.materialized.bTokEnd = counterBTokenRange->second;
  }
  MacroPatchProof proof = planner_->GetProofLattice().MakeMacroPatchProof(
      MacroPatchProofKind::CounterLiteral,
      /*preservesInvocationStructure=*/false, m.id);
  CounterStateWitness counterState;
  counterState.hasCounterEvents = true;
  counterState.counterOrderKnown = true;
  counterState.suffixStateStable = true;
  counterState.literalizationStable = true;
  counterState.counterConsumptionCount = 1;
  counterState.counterMutationCount = 1;
  if (counterWitness.kind != SuffixStabilityWitnessKind::None)
    counterState.preservedSuffixObserverCount = 1;
  counterState.hasExpectedBValues = true;
  counterState.expectedBValueCount = 1;
  counterState.consumptionSignature =
      planner_->GetOwnerStateProof().FormatCounterEventForWitness(event);
  counterState.orderSignature =
      llvm::formatv("ordinal={0}:macro={1}:A=[{2},{3})",
                    event.occurrenceOrdinal, event.macroInvocationId,
                    event.aTokenBegin, event.aTokenEnd)
          .str();
  counterState.suffixValueSignature =
      llvm::formatv("expected={0}", RefoldWitnessTrace::FormatWitnessTraceHash(
                                        *event.expectedBValue))
          .str();
  counterState.suffixObserverSignature =
      llvm::formatv("literalization:{0}", counterWitness.kind).str();
  proof.suffixStability = std::move(counterWitness);
  proof.counterState = std::move(counterState);
  planner_->GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
  return patch;
}

std::optional<MacroPatch>
RefoldMacroWholeCoverOrchestrator::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap) const {
  return BuildMacroInvocationPatchWholeCover(m, h, baseInvText, patchMap,
                                             ExistingMacroPatchContext{});
}

std::optional<MacroPatch>
RefoldMacroWholeCoverOrchestrator::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap,
    RefoldMacroPatchPlanner::ExistingMacroPatchContext existingContext) const {
  // Strategy (in priority order):
  //   1) Prefer an args-only rewrite of the invocation spelling when the edit
  //      is fully contained within argument-like spans.
  //   2) For function-like macros, attempt conservative DAG-chained args-only
  //      lifting from a nested callee invocation back to this callsite.
  //   3) Admit whole-cover realization only through the explicit
  //      RefoldMacroWholeCoverProof::MacroWholeCoverIsSelfContained() +
  //      OwnerRealizationProof path.
  // The invocation byte span in the owning file must be known.
  const auto invStart = m.invB;
  const auto invEnd = m.invE;
  if (!invStart || !invEnd || *invEnd < *invStart)
    return std::nullopt;

  // Special-case: __COUNTER__.
  if (auto counterPatch =
          TryCounterLiteralWholeCoverPatch(m, h, *invStart, *invEnd))
    return counterPatch;

  // Bundle the per-call planning state into a named carrier
  // (`RefoldMacroWholeCoverPlanningContext`) so each phase service takes a
  // single argument instead of re-expanding the parameter list.  The
  // locals below still hold the canonical storage — the carrier borrows
  // them by reference — and the rest of this method continues to read the
  // same names as before the carrier existed.
  const Owner currentPatchOwner = planner_->NormalizeHunkOwnerForPatch(
      (*planner_->Deps().model).GetSourcePath(), h);
  const diffutils::Hunk hEff =
      trimCommonEdgeTokens(h, planner_->Deps().aToks, planner_->Deps().bToks);
  MacroPatchReuseAdmissionContext reuseAdmissionCtx =
      planner_->RecoverWholeCoverReuseContext(
          m, currentPatchOwner, *invStart, *invEnd, patchMap, existingContext);
  SmallVector<RefoldModel::PPArgSpan, 16> argLikeSpans;
  argLikeSpans.append(m.argSpans.begin(), m.argSpans.end());
  argLikeSpans.append(m.stringifySpans.begin(), m.stringifySpans.end());
  argLikeSpans.append(m.pasteSpans.begin(), m.pasteSpans.end());

  RefoldMacroWholeCoverPlanningContext planningCtx{
      /*m=*/m,
      /*h=*/h,
      /*hEff=*/hEff,
      /*baseInvText=*/baseInvText,
      /*patchMap=*/patchMap,
      /*existingContext=*/existingContext,
      /*currentPatchOwner=*/currentPatchOwner,
      /*invStart=*/*invStart,
      /*invEnd=*/*invEnd,
      /*argLikeSpans=*/argLikeSpans,
      /*reuseAdmissionCtx=*/reuseAdmissionCtx,
      /*argsOnlyCandidate=*/std::nullopt,
      /*dagRootCandidate=*/std::nullopt,
      /*conflictingConcreteSubtreeWitnessForcesWholeCover=*/false,
      /*reuseExistingCallsitePatch=*/false,
      /*existingCallsitePatchAbsorbedByDirectCandidate=*/false,
      /*rootHasDirectArgLikeSurface=*/false};

  // Aliases bound to planning-context mutable state so the rest of the
  // orchestrator body can read the fields that phase services write into the
  // shared planning context.
  bool &conflictingConcreteSubtreeWitnessForcesWholeCover =
      planningCtx.conflictingConcreteSubtreeWitnessForcesWholeCover;
  std::optional<MacroPatch> &argsOnlyCandidate = planningCtx.argsOnlyCandidate;
  std::optional<MacroPatch> &dagRootCandidate = planningCtx.dagRootCandidate;
  bool &reuseExistingCallsitePatch = planningCtx.reuseExistingCallsitePatch;

  // Prefer args-only patching when safe and fully validated. Treat normal arg
  // spans, stringify spans, and paste spans as argument-like occurrences.  The
  // args-only phase reads m/hEff/baseInvText/argLikeSpans/reuseAdmissionCtx
  // from `planningCtx` and writes `argsOnlyCandidate`,
  // `reuseExistingCallsitePatch`, and `rootHasDirectArgLikeSurface` back into
  // that context.
  argsOnlyPhase_.Run(planningCtx);

  // 1b) Conservative DAG chaining: if the edited A-span lies within this
  //     invocation's cover but not within one of its direct argument-like
  //     spans, attempt to lift the edit from a nested callee invocation back
  //     to this callsite's arguments.
  //
  // Policy:
  //   * Every intermediate hop must be proven by arg_refs/template inversion.
  //   * A hop is allowed whenever the child argument text admits a unique
  //     inverse through arg_refs back to the contributing caller formals.
  //   * If the inverse is ambiguous, unsupported, or does not match the
  //     observed text, lifting fails and we conservatively keep the subtree
  //     expanded.
  if (m.subkind == "func" && hasLiteralMacroCalleeOrigin(m)) {
    auto tryDAGChainedArgsOnly = [&]() -> std::optional<MacroPatch> {
      // The DAG chain runs in two phase services.  Discovery computes the
      // leaf candidate set, the split-insertion root candidates, and the
      // per-formal root argument cache; the lifting phase consumes that
      // result and produces the final lifted root patch.  The
      // `directRootPreservationInadmissible` flag is later consumed by
      // the final candidate selector, so it is propagated through
      // `planningCtx` rather than a local.
      DAGLeafDiscoveryResult discoveryResult;
      dagLeafDiscoveryPhase_.Run(planningCtx, discoveryResult);
      if (discoveryResult.aborted)
        return std::nullopt;
      planningCtx.directRootPreservationInadmissible =
          discoveryResult.directRootPreservationInadmissible;
      return dagLiftingPhase_.Run(planningCtx, discoveryResult);
    };

    // Call-chain suffix patch: if this hunk's A-side PP tokens map to source
    // bytes in the chained-call suffix immediately following this invocation
    // (e.g. currying-style chains like GET_MATH(ADD)(10)(20)), patch those
    // bytes directly. This preserves the call chain and avoids whole-cover
    // expansion.
    if (hasLiteralMacroCalleeOrigin(m) && m.invFile && m.invB && m.invE) {
      std::string invAbs =
          (*planner_->Deps().lineDirs).ToAbsolutePath(*m.invFile);
      auto bufOrErr = MemoryBuffer::getFile(invAbs);
      if (bufOrErr) {
        std::unique_ptr<MemoryBuffer> buf = std::move(*bufOrErr);
        StringRef invFileText = buf->getBuffer();
        const uint64_t n = invFileText.size();
        const uint64_t invEndAbs = *m.invE;
        if (invEndAbs <= n) {
          // Compute the source extent of the chained-call suffix after the
          // macro invocation. Only tokens that map into this suffix are
          // eligible for the local call-chain patch.
          const uint64_t chainEndAbs = stringutils::extendChainedCallEnd(
              invFileText, invEndAbs, StringRef());
          if (chainEndAbs > invEndAbs) {
            const uint64_t aLen = h.aEnd - h.aStart;
            const uint64_t bLen = h.bEnd - h.bStart;
            if (aLen == bLen && aLen > 0) {
              struct TokEdit {
                uint64_t bAbs;
                uint64_t eAbs;
                std::string repl;
              };
              SmallVector<TokEdit, 8> tokEdits;
              tokEdits.reserve(aLen);
              uint64_t minB = std::numeric_limits<uint64_t>::max();
              uint64_t maxE = 0;
              bool ok = true;

              // Re-map each changed A-side PP token back to its original source
              // byte range. Every token must come from the same invocation file
              // and lie wholly inside the chained-call suffix; otherwise this
              // hunk is not a local suffix rewrite.
              const auto &tokmapByPP =
                  (*planner_->Deps().model).GetTokmapByPP();
              for (uint64_t i = 0; i < aLen; ++i) {
                const uint64_t ppIdx = h.aStart + i;
                const uint64_t bTok = h.bStart + i;
                auto it = tokmapByPP.find(ppIdx);
                if (it == tokmapByPP.end()) {
                  ok = false;
                  break;
                }
                const RefoldModel::TokMapEntry &tm = it->second;
                if ((*planner_->Deps().lineDirs).ToAbsolutePath(tm.file) !=
                    invAbs) {
                  ok = false;
                  break;
                }
                if (tm.b < invEndAbs || tm.e > chainEndAbs) {
                  ok = false;
                  break;
                }
                if (tm.b > tm.e || tm.e > n) {
                  ok = false;
                  break;
                }
                StringRef repl = (*planner_->Deps().sourceMapper)
                                     .SliceBSource(bTok, bTok + 1);
                tokEdits.push_back(TokEdit{tm.b, tm.e, repl.str()});
                minB = std::min(minB, tm.b);
                maxE = std::max(maxE, tm.e);
              }

              if (ok && minB < maxE && maxE <= n) {
                std::string covered = invFileText.slice(minB, maxE).str();

                // Convert absolute source-token edits into offsets relative to
                // the minimal covered suffix slice. The patch will replace only
                // this local slice, not the whole root invocation.
                SmallVector<TextEdit, 8> edits;
                edits.reserve(tokEdits.size());
                for (const auto &te : tokEdits)
                  edits.push_back(TextEdit{te.bAbs - minB,
                                           te.eAbs - minB,
                                           te.repl,
                                           std::nullopt,
                                           std::nullopt,
                                           {},
                                           {},
                                           {}});
                llvm::sort(edits, [](const TextEdit &a, const TextEdit &b) {
                  return a.start < b.start;
                });

                // Apply edits in source order. No line-directive resync is
                // needed because this patch is confined to the chained-call
                // suffix slice and preserves the surrounding invocation text.
                std::string out;
                out.reserve(covered.size());
                uint64_t cur = 0;
                for (const TextEdit &e : edits) {
                  if (e.start < cur || e.end > covered.size()) {
                    ok = false;
                    break;
                  }
                  out.append(covered, cur, e.start - cur);
                  out.append(e.text);
                  cur = e.end;
                }
                if (ok) {
                  out.append(covered, cur, covered.size() - cur);
                  {
                    MacroPatch patch{minB, maxE, std::move(out), m.id};
                    planner_->GetProofLattice().SetMacroPatchProof(
                        patch,
                        planner_->GetProofLattice().MakeMacroPatchProof(
                            MacroPatchProofKind::CallChainSuffix,
                            /*preservesInvocationStructure=*/true, m.id));
                    return patch;
                  }
                }
              }
            }
          }
        }
      }
    }

    // Prefer the DAG result over a direct root args-only rewrite when both are
    // available: the DAG path has already proved a structure-preserving nested
    // inverse, while the direct root patch only proves expansion equality at
    // this callsite.

    // Keep the DAG root replay as a final-selection candidate instead of
    // returning it immediately.
    //
    // `tryDAGChainedArgsOnly()` can produce a structure-preserving root patch
    // that competes with the direct args-only root replay for the same
    // invocation span. When both candidates exist, this block first resolves
    // that local same-root competition using replay validation plus the proof
    // lattice. The winning DAG candidate is then carried into the common final
    // selector, where it can still compete against whole-cover realization and
    // any reusable already-tracked callsite patch.
    auto dag = tryDAGChainedArgsOnly();
    if (dag) {
      bool preferDirectRootCandidate = false;
      if (argsOnlyCandidate &&
          dag->invRange.begin == argsOnlyCandidate->invRange.begin &&
          dag->invRange.end == argsOnlyCandidate->invRange.end &&
          dag->replacement != argsOnlyCandidate->replacement &&
          !baseInvText.empty()) {
        // Both candidates target the same root invocation but produce different
        // text. Validate each replacement against the root invocation envelope
        // before asking the proof lattice to choose between their proof
        // classes.
        const bool directValid =
            patchReusePhase_.ValidateMergedDirectAndDagRootReplacement(
                m, baseInvText, StringRef(argsOnlyCandidate->replacement));
        const bool dagValid =
            patchReusePhase_.ValidateMergedDirectAndDagRootReplacement(
                m, baseInvText, StringRef(dag->replacement));

        // The normalized lattice comparator is authoritative for same-root
        // root-level competition. Once both candidates are individually valid,
        // choose the stronger compatible proof class by the explicit lattice
        // law rather than by an ad hoc direct-vs-DAG heuristic.
        if (directValid && dagValid) {
          const bool preferDirect =
              planner_->GetProofLattice().AcceptedResultRanker().LatticePrefers(
                  argsOnlyCandidate->proofSummary, dag->proofSummary);
          const bool preferDag =
              planner_->GetProofLattice().AcceptedResultRanker().LatticePrefers(
                  dag->proofSummary, argsOnlyCandidate->proofSummary);
          preferDirectRootCandidate = preferDirect || !preferDag;
        }
      } else if (argsOnlyCandidate &&
                 dag->replacement != argsOnlyCandidate->replacement) {
        // The candidates are not a clean same-span root competition, but they
        // still differ textually. Keep the trace explicit because the DAG path
        // will be staged below unless the direct candidate won above.
      }

      if (!preferDirectRootCandidate) {
        // Before staging the DAG patch, give it a chance to compose with an
        // existing structure-preserving callsite patch for the same root span.
        // A concrete subtree witness conflict suppresses the DAG candidate and
        // forces the later whole-cover path instead.
        patchReusePhase_.MergeCurrentRootWithExistingCallsitePatch(planningCtx,
                                                                   *dag);
        if (!conflictingConcreteSubtreeWitnessForcesWholeCover) {
          // The DAG candidate has already won the local same-root competition
          // against the direct args-only replay. Preserve that decision by
          // carrying only the DAG root candidate into the final selector; the
          // shared selector still arbitrates it against whole-cover and any
          // reusable already-tracked patch for the same invocation span.
          dagRootCandidate = *dag;
          argsOnlyCandidate.reset();
        } else {
          argsOnlyCandidate.reset();
          reuseExistingCallsitePatch = false;
        }
      }
    }

    // Try selector substitution only after ordinary DAG replay declined to
    // produce a root candidate.  When accepted, it enters the same final macro
    // candidate path as other DAG-root proofs so existing conflict handling and
    // lattice selection remain authoritative.
    if (!dagRootCandidate) {
      if (std::optional<MacroPatch> selectorPatch =
              selectorSubstitutionPhase_.Run(planningCtx)) {
        patchReusePhase_.MergeCurrentRootWithExistingCallsitePatch(
            planningCtx, *selectorPatch);
        if (!conflictingConcreteSubtreeWitnessForcesWholeCover) {
          dagRootCandidate = std::move(*selectorPatch);
          argsOnlyCandidate.reset();
        }
      }
    }
  }

  // Final candidate admission — post-discovery direct/existing merge,
  // whole-cover plan computation, and proof-lattice ranking — is owned by
  // `RefoldMacroFinalCandidateSelector`.  The selector reads the planning
  // context's candidate slots and conflict flags directly.
  return finalCandidateSelector_.Run(planningCtx);
}

} // namespace refold
} // namespace clang

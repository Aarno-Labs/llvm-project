//===--- RefoldMacroWholeCoverOrchestrator.cpp ------------------*- C++ -*-===//
//
// Whole-cover macro patch orchestrator for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroWholeCoverOrchestrator.h"

#include "edit/RefoldBInsertionLedger.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
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

/// Hard cap for walking caller_macro_id ancestors during generated-descendant
/// recursive tuple replay recovery.  Reaching the cap rejects the recovery
/// path rather than truncating producer ancestry.
constexpr unsigned RecursiveTupleAncestorDepthLimit = 64;

/// Return the physical source end consumed by an object-like macro whose
/// replacement names a function-like macro that immediately consumes the
/// call-suffix group following the object invocation.
///
/// The producer represents this preprocessing shape as an object invocation
/// with a direct function-like child whose `inv_text` is absent: the child is
/// not a separately spelled source callsite, because its callee came from the
/// parent replacement list.  The child's preprocessed invocation envelope is
/// therefore identical to the parent's, while its `inv_e` records the end of
/// the source-supplied argument group.  Whole-cover realization must replace
/// that complete physical envelope; replacing only the object token would
/// append the untouched source arguments to an already materialized call.
///
/// Every structural fact is required exactly and a unique child must certify
/// the parsed suffix endpoint.  Missing, conflicting, or ambiguous metadata
/// leaves the ordinary invocation endpoint unchanged.
static uint64_t resolveGeneratedFunctionCallSourceEnd(
    const RefoldMacroPatchPlanner &planner,
    const RefoldModel::MacroInvocation &invocation, uint64_t invocationEnd) {
  if (invocation.subkind != "obj" || !invocation.invFile ||
      !invocation.invPPByteBegin || !invocation.invPPByteEnd)
    return invocationEnd;

  const RefoldModel::MacroInvocation *generatedFunctionChild = nullptr;
  for (const RefoldModel::MacroInvocation &candidate :
       planner.Deps().model->GetMacroInvocations()) {
    if (!candidate.callerMacroId ||
        *candidate.callerMacroId != invocation.id ||
        candidate.subkind != "func" || candidate.invText || !candidate.invE ||
        candidate.invPPByteBegin != invocation.invPPByteBegin ||
        candidate.invPPByteEnd != invocation.invPPByteEnd)
      continue;

    if (generatedFunctionChild)
      return invocationEnd;
    generatedFunctionChild = &candidate;
  }

  if (!generatedFunctionChild || *generatedFunctionChild->invE <= invocationEnd)
    return invocationEnd;

  const std::string invocationPath =
      planner.Deps().lineDirs->ToAbsolutePath(*invocation.invFile);
  auto bufferOrError = MemoryBuffer::getFile(invocationPath);
  if (!bufferOrError)
    return invocationEnd;

  const StringRef invocationFileText = (*bufferOrError)->getBuffer();
  if (invocationEnd > invocationFileText.size())
    return invocationEnd;

  const uint64_t parsedSuffixEnd = stringutils::extendChainedCallEnd(
      invocationFileText, invocationEnd, StringRef());
  if (parsedSuffixEnd != *generatedFunctionChild->invE)
    return invocationEnd;

  return parsedSuffixEnd;
}

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
              planner_->Deps().aToks, planner_->Deps().bToks,
              *planner_->Deps().proofLattice,
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
          planner_->Deps().ownersMustExpand,
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
  // Use the token-level projection here: whole-cover replaces the invocation's
  // entire expansion envelope, and the byte-level mapper can drift the envelope
  // start into an unrelated statement insertion that happens to precede a
  // re-materialized value token (repeated `int `/` = ` byte runs), causing the
  // realized text to swallow the inserted tokens.
  auto bEnv = (*planner_->Deps().sourceMapper)
                  .MapATokRangeToBTokenEnvelopeByTokenDiff(plan.covLoA,
                                                           plan.covHiA);
  if (!bEnv)
    return std::nullopt;

  plan.rawBTokStart = bEnv->first;
  plan.rawBTokEnd = bEnv->second;
  plan.bTokStart = plan.rawBTokStart;
  plan.bTokEnd = plan.rawBTokEnd;
  if (plan.bTokEnd <= plan.bTokStart)
    return std::nullopt;

  // The raw envelope from MapATokRangeToBTokenEnvelopeByTokenDiff is the
  // certified whole-cover image: the token-level diff is atomic and already
  // anchors a genuine boundary insertion exactly at the cover's start/end
  // token, and downstream claim-clipping removes any B-only payload that a
  // stronger/narrower candidate owns.  We deliberately do NOT nudge the seam by
  // spelling equality with a neighboring B token: a repeated spelling is not
  // provenance (a whole-cover expansion such as `"[" #x "]"` legitimately
  // repeats the argument spelling), so a spelling-based ±1 shift can split the
  // envelope at an uncertified token match and drop a real cover token.  The
  // seam therefore stays exactly where the certified projection placed it.
  plan.adjustedLeft = false;
  plan.adjustedRight = false;

  // Claim-clip the whole-cover B envelope: any boundary insertion already
  // claimed by a stronger/narrower candidate is dropped so it is not emitted
  // twice.  The surviving B tokens must form exactly one contiguous run; fail
  // closed (offer no whole-cover candidate) otherwise, because:
  //   * more than one kept segment means a claimed edit was removed from the
  //     *interior* of the envelope, so the emitted source would splice the two
  //     surviving sides directly together -- an unproven join that can paste
  //     adjacent tokens and represents a foreign edit conflicting inside this
  //     macro's realized expansion; and
  //   * zero kept segments means the whole envelope was claimed away, leaving
  //     nothing for this cover to materialize.
  // Whole-cover has no proof that either shape reproduces the cover's B tokens,
  // so a weaker sound path (or the conservative fallback) must handle the
  // region.  This is a self-contained soundness gate: it does not rely on the
  // downstream global certifier to reject an unsound splice after the fact.
  llvm::SmallVector<std::pair<size_t, size_t>, 4> keptSegments =
      planner_->Deps().bInsertionLedger->ClipBTokenRangeAgainstClaims(
          plan.bTokStart, plan.bTokEnd);
  if (keptSegments.size() != 1)
    return std::nullopt;

  // The single kept segment is the sound whole-cover B image.  It equals the
  // raw envelope exactly when nothing was claimed away; a narrower segment means
  // this candidate yielded a boundary insertion to an existing claim.
  const std::pair<size_t, size_t> &keptSegment = keptSegments.front();
  plan.claimsClipped = (keptSegment.first != plan.bTokStart ||
                        keptSegment.second != plan.bTokEnd);

  // Materialize the kept segment from the bytes its B tokens occupy, not from
  // the first byte of the token that follows it.  The trailing trivia is not
  // part of what this cover realizes, and after sideband pragma normalization
  // it can hold a whole directive line: the sideband path removed those lines
  // from the token stream and pairs each one with the source directive that
  // produced it, so carrying the bytes here would emit the directive a second
  // time while that source directive still stands.  The realized text is
  // trimmed either way, so this changes nothing for a range whose trailing
  // trivia is whitespace.
  std::optional<StringRef> material =
      (*planner_->Deps().sourceMapper)
          .SliceBTokenMaterial(keptSegment.first, keptSegment.second);
  if (!material)
    return std::nullopt;
  plan.clippedText = material->trim().str();

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

  // Both branches realize the literal from the bytes their B tokens occupy;
  // see ComputeWholeCoverPlan for why the trivia after the last token is not
  // this range's to carry.
  std::optional<std::string> repl;
  std::optional<std::pair<uint64_t, uint64_t>> counterBTokenRange;
  if (h.bEnd > h.bStart) {
    counterBTokenRange = std::make_pair(static_cast<uint64_t>(h.bStart),
                                        static_cast<uint64_t>(h.bEnd));
    if (std::optional<StringRef> material =
            (*planner_->Deps().sourceMapper)
                .SliceBTokenMaterial(static_cast<size_t>(h.bStart),
                                     static_cast<size_t>(h.bEnd)))
      repl = material->trim().str();
  } else {
    auto bEnv = (*planner_->Deps().sourceMapper)
                    .MapATokRangeAToBTokenEnvelope(h.aStart, h.aEnd);
    if (bEnv && bEnv->second > bEnv->first) {
      counterBTokenRange = std::make_pair(static_cast<uint64_t>(bEnv->first),
                                          static_cast<uint64_t>(bEnv->second));
      if (std::optional<StringRef> material =
              (*planner_->Deps().sourceMapper)
                  .SliceBTokenMaterial(bEnv->first, bEnv->second))
        repl = material->trim().str();
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
RefoldMacroWholeCoverOrchestrator::TryRecursiveTupleGeneratedReplayFromCallerAncestor(
    const RefoldModel::MacroInvocation &invocation,
    const diffutils::Hunk &hunk) const {
  if (!invocation.callerMacroId)
    return std::nullopt;

  auto findUniqueInvocationById =
      [this](uint64_t invocationId) -> const RefoldModel::MacroInvocation * {
    const RefoldModel::MacroInvocation *found = nullptr;
    for (const RefoldModel::MacroInvocation &candidate :
         planner_->Deps().model->GetMacroInvocations()) {
      if (candidate.id != invocationId)
        continue;
      if (found)
        return nullptr;
      found = &candidate;
    }
    return found;
  };

  DenseSet<uint64_t> visitedInvocationIds;
  visitedInvocationIds.insert(invocation.id);

  std::optional<MacroPatch> uniqueCandidate;
  const RefoldModel::MacroInvocation *cursor = &invocation;
  for (unsigned depth = 0; depth < RecursiveTupleAncestorDepthLimit; ++depth) {
    if (!cursor->callerMacroId)
      break;
    if (!visitedInvocationIds.insert(*cursor->callerMacroId).second)
      return std::nullopt;

    const RefoldModel::MacroInvocation *ancestor =
        findUniqueInvocationById(*cursor->callerMacroId);
    if (!ancestor)
      return std::nullopt;
    cursor = ancestor;

    if (!ancestor->invText || !ancestor->invB || !ancestor->invE)
      continue;
    if (ancestor->calleeOrigin.kind != MacroCalleeOriginKind::LiteralMacroName)
      continue;
    if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
            *ancestor->invText, *ancestor))
      continue;

    std::optional<std::pair<uint64_t, uint64_t>> ancestorCover =
        RefoldMacroWholeCoverProof::GetWholeCoverATokRange(*ancestor);
    if (!ancestorCover || hunk.aStart < ancestorCover->first ||
        hunk.aEnd > ancestorCover->second)
      continue;

    std::optional<MacroPatch> candidate =
        planner_->BuildMacroInvocationPatchArgsOnly(*ancestor, hunk,
                                                   *ancestor->invText);
    if (!candidate)
      continue;

    // The ancestor probe is only for the recursive tuple-generated-callee
    // theorem.  Do not let an ordinary args-only or realization proof discovered
    // while walking upward escape under a descendant planning frame.
    if (candidate->proof.kind !=
            MacroPatchProofKind::RecursiveTupleGeneratedCalleeReplay ||
        !candidate->proof.preservesInvocationStructure ||
        candidate->proof.proofRootMacroId != ancestor->id ||
        candidate->macroId != ancestor->id)
      continue;

    std::optional<std::vector<std::pair<size_t, size_t>>> ancestorArgRanges =
        planner_->GetMacroInvocationFormalArgContentRanges(*ancestor,
                                                           *ancestor->invText);
    if (!ancestorArgRanges)
      continue;

    DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invocationById;
    invocationById.reserve(planner_->Deps().model->GetMacroInvocations().size());
    for (const RefoldModel::MacroInvocation &modelInvocation :
         planner_->Deps().model->GetMacroInvocations())
      invocationById[modelInvocation.id] = &modelInvocation;

    const MacroSubtreeReplayValidationContext replayCtx{
        *ancestor, *ancestor->invText,
        ArrayRef<std::pair<size_t, size_t>>(ancestorArgRanges->data(),
                                            ancestorArgRanges->size()),
        invocationById};
    if (!planner_->ReplayStabilityValidator()
             .MacroCandidateReplayIsStableForFinalSelection(replayCtx,
                                                            *candidate))
      continue;

    if (uniqueCandidate)
      return std::nullopt;
    uniqueCandidate = std::move(candidate);
  }

  if (cursor->callerMacroId)
    return std::nullopt;

  return uniqueCandidate;
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
  if (!invStart || !invEnd || *invEnd < *invStart) {
    REFOLD_LOG_TRACE(
        "macro/whole-cover",
        "reject inv id={0} name={1} reason=missing-or-invalid-invocation-range invB={2} invE={3}",
        m.id, m.name, invStart ? std::to_string(*invStart) : std::string("<none>"),
        invEnd ? std::to_string(*invEnd) : std::string("<none>"));
    return std::nullopt;
  }

  REFOLD_LOG_TRACE(
      "macro/whole-cover",
      "enter inv id={0} name={1} subkind={2} hA=[{3},{4}) hB=[{5},{6}) "
      "invBytes=[{7},{8}) baseTextBytes={9} baseText='{10}'",
      m.id, m.name, m.subkind, h.aStart, h.aEnd, h.bStart, h.bEnd,
      *invStart, *invEnd, baseInvText.size(),
      stringutils::showWsWithClip(baseInvText, 220));

  // Special-case: __COUNTER__.
  if (auto counterPatch =
          TryCounterLiteralWholeCoverPatch(m, h, *invStart, *invEnd))
    return counterPatch;

  // Object-like aliases can generate a function-like macro invocation whose
  // arguments are supplied by the immediately following source suffix.  In
  // that producer shape, whole-cover realization owns both the alias token and
  // the consumed suffix; use the uniquely certified physical endpoint so the
  // untouched source arguments are not appended to the materialized call.
  const uint64_t effectiveInvEnd =
      resolveGeneratedFunctionCallSourceEnd(*planner_, m, *invEnd);

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

  // Generated function-like callees can be represented by producer pseudo-
  // invocations whose source span is not a real callsite, for example
  // `ADD, (1, 2)` inside `A(ADD, (1, 2))`.  Such descendants cannot enter the
  // ordinary callsite-shaped args-only phase, but they may still have an exact
  // recursive tuple-generated-callee proof rooted at a caller ancestor.  Try
  // that proof before owner realization can replace the pseudo-invocation
  // bytes with an expanded expression.
  if (m.subkind == "func") {
    StringRef invocationText =
        !baseInvText.empty() ? baseInvText
                             : (m.invText ? StringRef(*m.invText) : StringRef());
    if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
            invocationText, m)) {
      if (std::optional<MacroPatch> ancestorPatch =
              TryRecursiveTupleGeneratedReplayFromCallerAncestor(m, hEff))
        return ancestorPatch;
    }
  }

  MacroPatchReuseAdmissionContext reuseAdmissionCtx =
      planner_->RecoverWholeCoverReuseContext(
          m, currentPatchOwner, *invStart, effectiveInvEnd, patchMap,
          existingContext);
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
      /*invEnd=*/effectiveInvEnd,
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
  REFOLD_LOG_TRACE(
      "macro/whole-cover",
      "after-args-only inv id={0} name={1} argsOnly={2} reuseCallsite={3} "
      "rootHasDirectArgLikeSurface={4} directRootInadmissible={5}",
      m.id, m.name, argsOnlyCandidate ? 1 : 0,
      reuseExistingCallsitePatch ? 1 : 0,
      planningCtx.rootHasDirectArgLikeSurface ? 1 : 0,
      planningCtx.directRootPreservationInadmissible ? 1 : 0);

  // Definition-tape replay is intentionally ranked before the standard
  // args-only builder.  It can therefore admit a zero-token formal assignment
  // before the builder's higher-order generated-callee probe runs.  Give that
  // exact probe one competing chance only when an earlier direct args-only
  // candidate already exists.  This preserves the ordinary body-owned
  // whole-cover policy while allowing a stronger generated-callee proof to keep
  // the selector and generated actual-list structure intact.
  if (argsOnlyCandidate && m.subkind == "func") {
    // The higher-order theorem needs only the recovered formal-content ranges;
    // it reuses the same producer graph, old/new expansion solvers, uniqueness
    // checks, invocation rewrite builder, and proof certification as the
    // standard args-only path.  Missing or ambiguous recovery fails closed and
    // leaves the original args-only candidate unchanged.
    if (std::optional<std::vector<std::pair<size_t, size_t>>>
            higherOrderArgRanges =
                planner_->GetMacroInvocationFormalArgContentRanges(
                    m, baseInvText)) {
      if (std::optional<MacroPatch> higherOrderPatch =
              planner_->StandardArgsOnlyPatchBuilder()
                  .TryBuildHigherOrderGeneratedReplay(
                      m, h, baseInvText, *higherOrderArgRanges)) {
        // Only invocation-preserving generated replay can dominate an admitted
        // args-only candidate.  Generated-leaf materialization remains on the
        // existing whole-cover realization path.
        if (higherOrderPatch->proof.preservesInvocationStructure) {
          REFOLD_LOG_TRACE(
              "macro/whole-cover",
              "higher-order-generated-competitor-accepted inv id={0} "
              "name={1} proof={2} replacement='{3}'",
              m.id, m.name, toString(higherOrderPatch->proof.kind),
              stringutils::showWsWithClip(higherOrderPatch->replacement, 220));
          patchReusePhase_.MergeCurrentRootWithExistingCallsitePatch(
              planningCtx, *higherOrderPatch);
          if (!conflictingConcreteSubtreeWitnessForcesWholeCover) {
            // Generated-callee replay proves the edited descendant through the
            // root replacement-list grammar.  It is therefore stronger than a
            // generic zero-token args-only assignment that can erase the
            // selector formal, e.g. `CALL(SECOND, (, 1))` -> `CALL(, 2)`.
            argsOnlyCandidate.reset();
            dagRootCandidate = std::move(*higherOrderPatch);
            reuseExistingCallsitePatch = false;
          }
        }
      }
    }
  }

  // Literal sibling terminal tuple replay is more specific than ordinary
  // args-only assignment.  A root such as `BOTH(t) -> DECL t NAME t` can have
  // one terminal proving a tuple element through paste and another proving the
  // same element through stringification.  Ordinary args-only may update only a
  // later standard occurrence and leave the shared tuple selector stale, so try
  // the producer-backed sibling theorem before final selection can commit the
  // partial root rewrite.
  if (m.subkind == "func") {
    if (std::optional<MacroPatch> siblingTerminalPatch =
            planner_->TryBuildTupleSiblingTerminalReplayPatch(m,
                                                              baseInvText)) {
      REFOLD_LOG_TRACE(
          "macro/whole-cover",
          "sibling-tuple-terminal-accepted inv id={0} name={1} replacement='{2}'",
          m.id, m.name,
          stringutils::showWsWithClip(siblingTerminalPatch->replacement, 220));
      patchReusePhase_.MergeCurrentRootWithExistingCallsitePatch(
          planningCtx, *siblingTerminalPatch);
      if (!conflictingConcreteSubtreeWitnessForcesWholeCover) {
        argsOnlyCandidate.reset();
        dagRootCandidate = std::move(*siblingTerminalPatch);
        reuseExistingCallsitePatch = false;
      }
    }
  }

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
  if (!dagRootCandidate && m.subkind == "func" &&
      hasLiteralMacroCalleeOrigin(m)) {
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
    REFOLD_LOG_TRACE("macro/whole-cover",
                     "after-dag-probe inv id={0} name={1} dag={2} argsOnly={3}",
                     m.id, m.name, dag ? 1 : 0, argsOnlyCandidate ? 1 : 0);
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
      REFOLD_LOG_TRACE("macro/whole-cover",
                       "selector-probe-start inv id={0} name={1}", m.id,
                       m.name);
      if (std::optional<MacroPatch> selectorPatch =
              selectorSubstitutionPhase_.Run(planningCtx)) {
        REFOLD_LOG_TRACE(
            "macro/whole-cover",
            "selector-probe-accepted inv id={0} name={1} proof={2} replacement='{3}'",
            m.id, m.name, toString(selectorPatch->proof.kind),
            stringutils::showWsWithClip(selectorPatch->replacement, 220));
        patchReusePhase_.MergeCurrentRootWithExistingCallsitePatch(
            planningCtx, *selectorPatch);
        REFOLD_LOG_TRACE(
            "macro/whole-cover",
            "selector-after-merge inv id={0} name={1} conflictForcesWhole={2} reuseCallsite={3}",
            m.id, m.name,
            conflictingConcreteSubtreeWitnessForcesWholeCover ? 1 : 0,
            reuseExistingCallsitePatch ? 1 : 0);
        if (!conflictingConcreteSubtreeWitnessForcesWholeCover) {
          dagRootCandidate = std::move(*selectorPatch);
          argsOnlyCandidate.reset();
          REFOLD_LOG_TRACE(
              "macro/whole-cover",
              "selector-staged-as-dag-root inv id={0} name={1}", m.id,
              m.name);
        }
      } else {
        REFOLD_LOG_TRACE("macro/whole-cover",
                         "selector-probe-declined inv id={0} name={1}",
                         m.id, m.name);
      }
    } else {
      REFOLD_LOG_TRACE("macro/whole-cover",
                       "selector-probe-skipped inv id={0} name={1} reason=dag-root-already-present",
                       m.id, m.name);
    }
  }

  // Final candidate admission — post-discovery direct/existing merge,
  // whole-cover plan computation, and proof-lattice ranking — is owned by
  // `RefoldMacroFinalCandidateSelector`.  The selector reads the planning
  // context's candidate slots and conflict flags directly.
  REFOLD_LOG_TRACE(
      "macro/whole-cover",
      "before-final-selector inv id={0} name={1} argsOnly={2} dagRoot={3} "
      "reuseCallsite={4} conflictForcesWhole={5}",
      m.id, m.name, argsOnlyCandidate ? 1 : 0, dagRootCandidate ? 1 : 0,
      reuseExistingCallsitePatch ? 1 : 0,
      conflictingConcreteSubtreeWitnessForcesWholeCover ? 1 : 0);
  std::optional<MacroPatch> selectedPatch = finalCandidateSelector_.Run(planningCtx);
  REFOLD_LOG_TRACE(
      "macro/whole-cover",
      "after-final-selector inv id={0} name={1} selected={2} replacement='{3}'",
      m.id, m.name, selectedPatch ? 1 : 0,
      selectedPatch ? stringutils::showWsWithClip(selectedPatch->replacement, 220)
                    : std::string(""));
  return selectedPatch;
}

} // namespace refold
} // namespace clang

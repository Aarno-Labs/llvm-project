//===--- RefoldMacroWholeCoverOrchestrator.cpp ------------------*- C++ -*-===//
//
// Whole-cover macro patch orchestrator for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroWholeCoverOrchestrator.h"

#include "edit/RefoldBInsertionLedger.h"
#include "edit/RefoldEditTypes.h"
#include "line-control/LineDirectiveInserter.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroFormalDependenceProof.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroStandardArgsOnlyPatchBuilder.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroWholeCoverPlanBuilder.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"
#include "model/RefoldPathIdentity.h"
#include "proof/RefoldAcceptedResultRanker.h"
#include "proof/RefoldMacroPatchProofClassifier.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "support/StringUtils.h"

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

// Small token-spelling / PP-span helpers shared across the DAG phase
// services live in `macro/RefoldMacroDAGSharedHelpers.h`
// (`tokenSpellingVectorsEqual`, `hunkWithinPPSpans`,
// `ppSpanLessByTokenRange`, `ppArgSpanProofWidth`).  Include that header
// if you need them here.

// Build the orchestrator's owned phase services from the planner's public
// dependency bundle (`RefoldMacroPatchPlanner::Deps()`), the planner's
// sub-service accessors, and a small set of std::function callbacks that
// forward into planner-side helpers.  The planner constructs a temporary
// orchestrator for every whole-cover call, so everything built here is
// rebuilt per call; keep it to borrowed references and cheap lambdas.
RefoldMacroWholeCoverOrchestrator::RefoldMacroWholeCoverOrchestrator(
    const RefoldMacroPatchPlanner *planner)
    : planner_(planner),
      argsOnlyPhase_(RefoldMacroArgsOnlyWholeCoverPhase::Dependencies{
          *planner_->Deps().sourceMapper, planner_->Deps().aToks,
          planner_->Deps().bToks, *planner_->Deps().abTokHunks,
          *planner_->Deps().macroPatchProofClassifier,
          planner_->StandardArgsOnlyPatchBuilder()}),
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
          planner_->StandardArgsOnlyPatchBuilder()}),
      dagLiftingPhase_(RefoldMacroDAGLiftingPhase::Dependencies{
          *planner_->Deps().model, *planner_->Deps().sourceMapper,
          *planner_->Deps().lexLang, *planner_->Deps().lineDirs,
          *planner_->Deps().argTextRecovery, *planner_->Deps().macroTopology,
          planner_->Deps().aToks, planner_->Deps().bToks,
          *planner_->Deps().macroPatchProofClassifier,
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
          *planner_->Deps().wholeCoverPlanBuilder}),
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
              *planner_->Deps().macroPatchProofClassifier,
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
          *planner_->Deps().acceptedCandidateBuilder,
          *planner_->Deps().acceptedResultRanker,
          planner_->ReplayStabilityValidator(), planner_->ProofCertifier(),
          patchReusePhase_, planner_->Deps().ownersMustExpand,
          planner_->Deps().sidebandPragmaLinePairings,
          *planner_->Deps().wholeCoverPlanBuilder,
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
          },
          *planner_->Deps().lexLang, planner_->Deps().aToks,
          planner_->Deps().bToks, *planner_->Deps().abTokHunks}) {}

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
  // see RefoldMacroWholeCoverPlanBuilder::ComputeWholeCoverPlan for why the
  // trivia after the last token is not this range's to carry.
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
  certifyMacroPatchWhollyBTokens(patch, counterBTokenRange->first,
                                 counterBTokenRange->second);
  MacroPatchProof proof =
      makeMacroPatchProof(MacroPatchProofKind::CounterLiteral,
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
  planner_->Deps().macroPatchProofClassifier->SetMacroPatchProof(
      patch, std::move(proof));
  return patch;
}

/// Return whether \p inv's caller chain reaches \p ancestorId.  A chain
/// longer than the invocation count, or broken, does not.
static bool isCallerDescendantOf(const RefoldMacroTopology &topology,
                                 const RefoldModel::MacroInvocation &inv,
                                 uint64_t ancestorId, size_t invocationCount) {
  const RefoldModel::MacroInvocation *cur = &inv;
  for (size_t hops = 0; hops <= invocationCount && cur->callerMacroId; ++hops) {
    if (*cur->callerMacroId == ancestorId)
      return true;
    cur = topology.FindMacroInvocationById(*cur->callerMacroId);
    if (!cur)
      return false;
  }
  return false;
}

/// Return whether every A-token span \p inv records lies in the half-open
/// \p cover; an invocation credited with no token must be anchored there.
static bool
invocationProvenanceInsideCover(const RefoldModel::MacroInvocation &inv,
                                std::pair<uint64_t, uint64_t> cover) {
  for (const RefoldModel::PPSpan &span : inv.spans)
    if (span.begin < cover.first || span.end > cover.second)
      return false;
  for (const RefoldModel::PPSpan &span : inv.bodySpans)
    if (span.begin < cover.first || span.end > cover.second)
      return false;
  for (const std::vector<RefoldModel::PPArgSpan> *spans :
       {&inv.argSpans, &inv.stringifySpans, &inv.pasteSpans})
    for (const RefoldModel::PPArgSpan &span : *spans)
      if (span.begin < cover.first || span.end > cover.second)
        return false;
  return true;
}

/// Return whether the token hunk \p hunk edits the A-token range \p cover:
/// it replaces or deletes a token of it, or inserts between two of its
/// tokens.  An insertion at either edge is outside every token of the cover.
static bool hunkEditsCover(const diffutils::Hunk &hunk,
                           std::pair<uint64_t, uint64_t> cover) {
  if (hunk.aStart == hunk.aEnd)
    return cover.first < hunk.aStart && hunk.aStart < cover.second;
  return hunk.aStart < cover.second && cover.first < hunk.aEnd;
}

/// Return whether `use_arg_ranges` locates formal \p formal of \p inv in
/// source at or after \p suffixBegin.
static bool actualSpelledInSuffix(const RefoldModel::MacroInvocation &inv,
                                  uint32_t formal, uint64_t suffixBegin) {
  return formal < inv.useArgRanges.size() && inv.useArgRanges[formal].first &&
         *inv.useArgRanges[formal].first >= suffixBegin;
}

/// Return the standard and stringified occurrences of the formals of \p inv
/// whose actuals `use_arg_ranges` locates at or after \p suffixBegin.
static SmallVector<RefoldModel::PPArgSpan, 8>
suffixActualOccurrences(const RefoldModel::MacroInvocation &inv,
                        uint64_t suffixBegin) {
  SmallVector<RefoldModel::PPArgSpan, 8> occurrences;
  for (const std::vector<RefoldModel::PPArgSpan> *spans :
       {&inv.argSpans, &inv.stringifySpans})
    for (const RefoldModel::PPArgSpan &span : *spans)
      if (actualSpelledInSuffix(inv, span.argIdx, suffixBegin))
        occurrences.push_back(span);
  return occurrences;
}

/// Return whether \p inv, a translation-unit invocation other than the one
/// rewritten, has its name spelled in, or reads an actual from, the bytes
/// `[begin, end)` of the translation unit \p tuPath.
///
/// `inv_b` is where a call's name was spelled, but `inv_e` is where its last
/// token was, so the envelope of a call whose name a replacement list spelled
/// says nothing about the bytes between.  A call whose name is spelled in an
/// actual is expanded while that actual is, and is read nowhere else.
static bool invocationReadsSourceIn(const RefoldModel::MacroInvocation &inv,
                                    const RefoldPathIdentity &pathIdentity,
                                    StringRef tuPath, uint64_t begin,
                                    uint64_t end) {
  if (inv.ownerIncludeId)
    return false;
  if (inv.invFile && inv.invB &&
      pathIdentity.PathsEqual(*inv.invFile, tuPath) && begin <= *inv.invB &&
      *inv.invB < end)
    return true;
  return llvm::any_of(inv.useArgRanges,
                      [&](const RefoldModel::MacroInvocation::OptByteRange &r) {
                        return r.first && r.second && *r.first < end &&
                               begin < *r.second;
                      });
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
    if (!invocationSpanMatchesCallsitePrefix(*ancestor->invText, *ancestor))
      continue;

    std::optional<std::pair<uint64_t, uint64_t>> ancestorCover =
        RefoldMacroWholeCoverProof::GetWholeCoverATokRange(*ancestor);
    if (!ancestorCover || hunk.aStart < ancestorCover->first ||
        hunk.aEnd > ancestorCover->second)
      continue;

    std::optional<MacroPatch> candidate =
        planner_->StandardArgsOnlyPatchBuilder()
            .BuildMacroInvocationPatchArgsOnly(*ancestor, hunk,
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

/// Return whether \p text, an actual spelled from B, names a macro: one the
/// unit defines or undefines anywhere, or one Clang provides.
///
/// B is fully expanded, so such a name is an identifier there.  Spelled in
/// source as an actual it would be expanded when the actual is substituted,
/// and the unit-wide macro-state audit does not judge a macro patch's
/// payload, so the rewrite must not spell it at all.
static bool actualNamesMacro(StringRef text, const RefoldModel &model,
                             const clang::LangOptions &lexLang) {
  SmallVector<RefoldLexBoundaryToken, 16> tokens;
  refoldLexBoundaryTokens(text, lexLang, tokens);
  for (const RefoldLexBoundaryToken &token : tokens) {
    if (token.kind != tok::raw_identifier && token.kind != tok::identifier)
      continue;
    const StringRef name = token.spelling;
    if (!model.GetMacroDirectivesByName(name).empty() ||
        stringutils::isLineDirectiveSensitiveBuiltin(name) ||
        llvm::is_contained({StringRef("__COUNTER__"),
                            StringRef("__INCLUDE_LEVEL__"),
                            StringRef("__TIMESTAMP__"), StringRef("__DATE__"),
                            StringRef("__TIME__"), StringRef("_Pragma")},
                           name))
      return true;
  }
  return false;
}

/// Trace why the call-chain suffix theorem refused a hunk of \p root once a
/// call reading source after the root's callsite was found for it.
static void
traceCallChainSuffixRefusal(const RefoldModel::MacroInvocation &root,
                            uint64_t consumerId, StringRef reason) {
  REFOLD_LOG_TRACE("macro/call-chain-suffix",
                   "refuse root id={0} name={1} consumer id={2} reason={3}",
                   root.id, root.name, consumerId, reason);
}

std::optional<MacroPatch>
RefoldMacroWholeCoverOrchestrator::TryCallChainSuffixReplay(
    const RefoldModel::MacroInvocation &root,
    const diffutils::Hunk &hunk) const {
  const RefoldMacroPatchPlanner::Dependencies &deps = planner_->Deps();
  const RefoldModel &model = *deps.model;
  const StringRef tuPath = model.GetSourcePath();

  // The root's expansion forms the call, and the call reads source after the
  // root's callsite; only a translation-unit root atop its chain is tried.
  // An actual spelled inside the root's callsite is the root's own actual
  // material, which its args-only and DAG proofs rewrite.
  if (root.callerMacroId || root.ownerIncludeId || !root.invFile ||
      !root.invE || !deps.pathIdentity->PathsEqual(*root.invFile, tuPath))
    return std::nullopt;
  const uint64_t suffixBegin = *root.invE;
  if (deps.ownersMustExpand && deps.ownersMustExpand->contains(root.id))
    return std::nullopt;
  const std::optional<std::pair<uint64_t, uint64_t>> cover =
      planner_->GetWholeCoverATokRange(root);
  if (!cover || cover->first >= cover->second)
    return std::nullopt;

  // The one call, generated by the root, whose source-spelled actuals'
  // occurrences hold the hunk.
  const size_t invocationCount = model.GetMacroInvocations().size();
  const RefoldModel::MacroInvocation *consumer = nullptr;
  for (const RefoldModel::MacroInvocation &inv : model.GetMacroInvocations()) {
    if (inv.useArgRanges.empty() || inv.ownerIncludeId ||
        !isCallerDescendantOf(*deps.macroTopology, inv, root.id,
                              invocationCount))
      continue;
    const SmallVector<RefoldModel::PPArgSpan, 8> occurrences =
        suffixActualOccurrences(inv, suffixBegin);
    SmallVector<char, 8> touched(occurrences.size(), 0);
    if (occurrences.empty() ||
        !deps.sourceMapper->HunkFullyWithinArgSpans(hunk, occurrences, touched))
      continue;
    if (consumer) {
      traceCallChainSuffixRefusal(root, inv.id, "ambiguous-consumer");
      return std::nullopt;
    }
    consumer = &inv;
  }
  if (!consumer)
    return std::nullopt;

  // The rewrite replaces only the consumer's actuals, so it must explain every
  // edit to the root's cover, and the cover must hold every token the
  // consumer's expansion produced: the dependence check below reads only the
  // cover.
  SmallVector<diffutils::Hunk, 8> coverHunks;
  for (const diffutils::Hunk &h : *deps.abTokHunks)
    if (hunkEditsCover(h, *cover))
      coverHunks.push_back(trimCommonEdgeTokens(h, deps.aToks, deps.bToks));
  for (const RefoldModel::MacroInvocation &inv : model.GetMacroInvocations())
    if ((&inv == consumer ||
         isCallerDescendantOf(*deps.macroTopology, inv, consumer->id,
                              invocationCount)) &&
        !invocationProvenanceInsideCover(inv, *cover)) {
      traceCallChainSuffixRefusal(root, consumer->id,
                                  "consumer-output-outside-cover");
      return std::nullopt;
    }

  const std::string tuAbsPath = deps.lineDirs->ToAbsolutePath(tuPath);
  ErrorOr<std::unique_ptr<MemoryBuffer>> tuBuffer =
      MemoryBuffer::getFile(tuAbsPath);
  if (!tuBuffer) {
    traceCallChainSuffixRefusal(root, consumer->id, "unreadable-source");
    return std::nullopt;
  }
  const StringRef tuText = (*tuBuffer)->getBuffer();

  SmallVector<RefoldMacroStandardArgsOnlyPatchBuilder::SourceSpelledActual, 4>
      actuals;
  std::vector<std::pair<size_t, size_t>> actualRanges(
      consumer->useArgRanges.size(), {0, 0});
  for (uint32_t formal = 0; formal < consumer->useArgRanges.size(); ++formal) {
    const RefoldModel::MacroInvocation::OptByteRange &range =
        consumer->useArgRanges[formal];
    if (!actualSpelledInSuffix(*consumer, formal, suffixBegin) || !range.second)
      continue;
    if (*range.second > tuText.size()) {
      traceCallChainSuffixRefusal(root, consumer->id, "actual-past-source");
      return std::nullopt;
    }
    actuals.push_back({formal, tuText.slice(*range.first, *range.second)});
    actualRanges[formal] = {*range.first, *range.second};
  }
  const std::optional<DenseMap<uint32_t, std::string>> rewrites =
      planner_->StandardArgsOnlyPatchBuilder().DeriveSourceActualRewrites(
          *consumer, actuals, coverHunks);
  if (!rewrites) {
    traceCallChainSuffixRefusal(root, consumer->id, "actual-not-derived");
    return std::nullopt;
  }
  const SmallVector<uint32_t, 4> rewritten =
      RefoldMacroFormalDependenceProof::RewrittenFormals(
          tuText, actualRanges, *rewrites, *deps.lexLang);
  if (rewritten.empty()) {
    traceCallChainSuffixRefusal(root, consumer->id, "no-actual-respelled");
    return std::nullopt;
  }

  // A rewritten actual must still end where the old one did: one actual of
  // the call, or a whole variadic tail, with nothing after its list.  Its
  // tokens must reach the output as B spells them.
  for (uint32_t formal : rewritten) {
    if (actualNamesMacro(rewrites->lookup(formal), model, *deps.lexLang)) {
      traceCallChainSuffixRefusal(root, consumer->id, "actual-names-macro");
      return std::nullopt;
    }
    const std::string probe =
        (consumer->name + "(" + rewrites->lookup(formal) + ")").str();
    bool trailingTokens = false;
    const std::optional<std::vector<std::pair<size_t, size_t>>> parsed =
        RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
            probe, *deps.lexLang, &trailingTokens);
    if (!parsed || trailingTokens ||
        (!isMacroInvocationVariadicFormal(*consumer, formal) &&
         parsed->size() > 1)) {
      traceCallChainSuffixRefusal(root, consumer->id, "actual-boundary-moved");
      return std::nullopt;
    }
  }

  // No other token of the cover may observe a rewritten formal.  The
  // occurrences that do were replayed above.
  SmallVector<uint64_t, 32> keptTokens;
  for (uint64_t tok = cover->first; tok < cover->second; ++tok)
    if (llvm::none_of(coverHunks, [tok](const diffutils::Hunk &h) {
          return h.aStart <= tok && tok < h.aEnd;
        }))
      keptTokens.push_back(tok);
  if (!RefoldMacroFormalDependenceProof({model, *deps.macroTopology})
           .TokensInCoverIgnoreRewrittenFormals(
               *consumer, *cover, keptTokens, rewritten,
               RefoldMacroFormalDependenceProof::RootOccurrences::Validated)) {
    traceCallChainSuffixRefusal(root, consumer->id,
                                "cover-token-observes-rewrite");
    return std::nullopt;
  }

  // The rewritten bytes are the consumer's alone and hold no structure.
  const uint64_t patchBegin = actualRanges[rewritten.front()].first;
  const uint64_t patchEnd = actualRanges[rewritten.back()].second;
  for (const RefoldModel::MacroInvocation &inv : model.GetMacroInvocations())
    if (&inv != consumer &&
        invocationReadsSourceIn(inv, *deps.pathIdentity, tuPath, patchBegin,
                                patchEnd)) {
      traceCallChainSuffixRefusal(root, consumer->id,
                                  "call-spelled-in-rewritten-bytes");
      return std::nullopt;
    }
  if (!deps.wholeCoverPlanBuilder->SourceRangeHoldsNoStructure(
          tuPath, std::nullopt, patchBegin, patchEnd)) {
    traceCallChainSuffixRefusal(root, consumer->id,
                                "structure-in-rewritten-bytes");
    return std::nullopt;
  }

  std::string replacement;
  uint64_t cursor = patchBegin;
  for (uint32_t formal : rewritten) {
    replacement += tuText.slice(cursor, actualRanges[formal].first);
    replacement += rewrites->lookup(formal);
    cursor = actualRanges[formal].second;
  }
  const uint64_t rewrittenOutputEnd = replacement.size();
  MacroPatch patch{patchBegin, patchEnd, std::move(replacement), root.id};
  planner_->ProofCertifier().CertifyInvocationRewriteMaterializedOutputRange(
      patch, /*outputByteStart=*/0, rewrittenOutputEnd);
  // The B tokens the patch realizes are the image of the root's whole cover,
  // as for any rewrite that keeps the root's callsite.
  const std::optional<std::pair<size_t, size_t>> bEnvelope =
      deps.sourceMapper
          ->MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
              cover->first, cover->second);
  if (!bEnvelope || bEnvelope->first >= bEnvelope->second) {
    traceCallChainSuffixRefusal(root, consumer->id, "no-b-envelope");
    return std::nullopt;
  }
  patch.materialized.hasBTokenRange = true;
  patch.materialized.bTokStart = bEnvelope->first;
  patch.materialized.bTokEnd = bEnvelope->second;
  MacroPatchProof proof =
      makeMacroPatchProof(MacroPatchProofKind::CallChainSuffix,
                          /*preservesInvocationStructure=*/true, root.id);
  CallChainWitness callChainWitness;
  callChainWitness.rootMacroId = root.id;
  callChainWitness.callsiteMacroId = root.id;
  proof.callChain = callChainWitness;
  deps.macroPatchProofClassifier->SetMacroPatchProof(patch, std::move(proof));
  REFOLD_LOG_TRACE("macro/call-chain-suffix",
                   "accept root id={0} name={1} consumer id={2} name={3} "
                   "bytes=[{4},{5}) replacement='{6}'",
                   root.id, root.name, consumer->id, consumer->name, patchBegin,
                   patchEnd,
                   stringutils::showWsWithClip(patch.replacement, 220));
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
    ExistingMacroPatchContext existingContext) const {
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

  // An edit to the actuals a call formed by this root's expansion read from
  // source after the root's callsite is made there, keeping every callsite.
  if (std::optional<MacroPatch> suffixPatch = TryCallChainSuffixReplay(m, hEff))
    return suffixPatch;

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
    if (!invocationSpanMatchesCallsitePrefix(invocationText, m)) {
      if (std::optional<MacroPatch> ancestorPatch =
              TryRecursiveTupleGeneratedReplayFromCallerAncestor(m, hEff))
        return ancestorPatch;
    }
  }

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
              planner_->Deps().acceptedResultRanker->ProofDominates(
                  argsOnlyCandidate->proofSummary, dag->proofSummary);
          const bool preferDag =
              planner_->Deps().acceptedResultRanker->ProofDominates(
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

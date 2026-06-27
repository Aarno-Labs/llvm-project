//===--- RefoldEngine.cpp ---------------------------------------*- C++ -*-===//
//
// This component implements the deterministic “refolding” engine that projects
// edits made to a raw preprocessed stream (B) back onto the original, partially
// expanded translation unit (TU) described by the refold map.
//
// Overview
// --------
// RefoldEngine consumes:
//   • A: original preprocessed bytes and tokens
//   • B: edited preprocessed bytes and tokens
//   • M: RefoldModel (parsed from the JSON refold map)
//
// It aligns A↔B token streams, derives edit hunks, classifies each hunk as
// TU-owned / include-owned / macro-invocation–owned, and materializes a new
// TU that incorporates edits while preserving original structure and semantics.
//
// Strict-Domain Theorem
// ---------------------
// The implementation is organized around the theorem vocabulary declared in the
// focused proof/witness carrier headers.  An emitted edit is either an in-domain
// accepted result with a declared AcceptedProofClass, or an explicit terminal
// out-of-domain result with a named TerminalFallbackProofFailure.  No
// implementation-origin “fallback worked” path is allowed to stand in for a
// theorem-facing proof.
//
// In-domain completeness is restricted to finite deterministic owner-closed
// edit tilings whose state-transition summaries compose and whose preserved
// suffix observers are state-equivalent to B.  All other edits must fail closed
// by materializing a closed owner surface when possible or by emitting the
// named terminal out-of-domain carrier.
//
// Responsibilities
// ----------------
//   • Compute LCS-based A→B anchors and contiguous edit hunks.
//   • Attribute hunks to includes or macro call sites using M’s coverage data.
//   • Normalize and coalesce include insertions (line-local, boundary safe).
//   • Realize include expansions bottom-up, applying macro patches in-owner.
//   • Apply TU-level replacements with boundary hygiene (no token gluing).
//
// Determinism & Policy
// --------------------
//   • All iteration and sorting are stable; edits apply high→low to avoid
//     byte-offset drift.
//   • Boundary padding inserts at most one space locally when needed by
//     maximal-munch rules; internal whitespace is preserved verbatim.
//   • Errors are reported via the Logging subsystem (`fatal()/error()/...`).
//
// Public Surface
// --------------
//   • std::string Refold(...): orchestrates the end-to-end refolding and
//     returns the refolded TU text.
//   • Helper utilities: token/byte mapping, hunk builders, include realization,
//     macro-patch construction, and line-local boundary checks.
//
// Notes
// -----
//   • No RTTI or exceptions required; mirrors LLVM/Clang style.
//   • Paths are compared via the RefoldPathIdentity canonicalization service.
//   • All indices are half-open where applicable: tokens [lo,hi), bytes [b,e).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "core/RefoldLog.h"
#include "core/RefoldEngine.h"
#include "core/RefoldOwnerClassifier.h"
#include "edit/RefoldBInsertionLedger.h"
#include "edit/RefoldExpansionFallbackPlanner.h"
#include "edit/RefoldFinalTUEmissionPlanner.h"
#include "edit/RefoldTUAnchorProof.h"
#include "edit/RefoldTUEditPlanner.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/IncludeSpellingHelpers.h"
#include "include/RefoldIncludeMaterializationScheduler.h"
#include "include/RefoldIncludeMaterializer.h"
#include "include/RefoldIncludeReplayProof.h"
#include "include/RefoldSourceGraphProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/RefoldLineControlProof.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldCounterStabilization.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroStateRepairPlanner.h"
#include "macro/RefoldMacroTextUtils.h"
#include "proof/NeutralSourceIslandProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldMixedOwnerTilingPlanner.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "source/RefoldTokenDiffPlanner.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <set>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldEngine::RefoldEngine(RefoldModel model, StringRef aSource,
                               ArrayRef<PPTok> aToks,
                               ArrayRef<size_t> aTokOff, StringRef bSource,
                               ArrayRef<PPTok> bToks,
                               ArrayRef<size_t> bTokOff, bool noLines,
                               bool strict, ProofAuditMode proofAuditMode,
                               StringRef finalOutputPath,
                               ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
                               std::vector<MaterializedEditMapping>
                                   *materializedEditMappings,
                               FinalLineControlValidationCallback
                                   finalLineControlValidationCallback,
                               std::vector<SourceGraphOutput>
                                   *sourceGraphOutputs)
    : model_(std::move(model)), aSource_(aSource), bSource_(bSource),
      aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff),
      lineDirs_(!noLines, model_.GetPPCwd()),
      pathIdentity_(model_, model_.GetPPCwd(), /*emitAbsPaths=*/false),
      strict_(strict), proofAuditMode_(proofAuditMode),
      lexLang_(MakeLexLangOptions(model_.GetPPLang())),
      argTextRecovery_(lexLang_),
      tokenTextAnalysis_(lexLang_),
      terminalSink_(RefoldTerminalProofSinkCallbacks{
          [this](const TerminalFallbackProofFailure &failure, StringRef role) {
            TheoremAudit().AuditTerminalFallbackForLegacyAuthority(failure,
                                                                   role);
          },
          [this](StringRef detail) {
            TheoremAudit().NoteTheoremAuditViolation(detail);
          },
          [this](const TerminalFallbackRequest &request) {
            ProofLattice().TraceWitnessFallback(request);
          }}),
      finalReplaySurface_(buildFinalReplaySurface(model_, finalOutputPath)),
      materializedEditMappings_(materializedEditMappings),
      sourceGraphOutputs_(sourceGraphOutputs),
      finalLineControlValidationCallback_(
          std::move(finalLineControlValidationCallback)),
      sidebandPragmaEdits_(sidebandPragmaEdits.begin(),
                           sidebandPragmaEdits.end()),
      sourceMapper_(aSource_, bSource_, aToks_, bToks_, aTokOff_, bTokOff_,
                    abTokHunks_, abByteHunks_, abByteHunkPrefixDelta_, strict_),
      macroTopology_(model_, aToks_, bToks_, sourceMapper_, pathIdentity_),
      macroBoundarySelector_(model_, macroTopology_, sourceMapper_, bToks_, lexLang_),
      lineControlProof_(model_, sourceMapper_, pathIdentity_, tokenTextAnalysis_,
                        macroTopology_, lineDirs_, aToks_, bToks_, abTokMapA2B_,
                        abTokMapB2A_) {
  // Build the object graph in dependency order.  Each extracted service receives
  // explicit inputs/services and, where legacy orchestration is still owned by
  // RefoldEngine, a narrow hook bundle; no service stores RefoldEngine itself.
  InitializeTheoremAudit();
  InitializeOwnerStateProof();
  InitializeMacroStateProof();
  InitializeTokenDiffPlanner();
  InitializeTUAnchorProof();
  InitializeTUEditPlanner();
  InitializeOwnerClassifier();
  InitializeMixedOwnerTilingPlanner();
  InitializeBInsertionLedger();
  InitializeCounterStabilization();
  InitializeProofLattice();
  InitializeMacroPatchPlanner();
  InitializeIncludeInsertionPlanner();
  InitializeTextEditAssembler();
  InitializeMacroStateRepairPlanner();
  InitializeLineObserverLayout();
  InitializeIncludeMaterializer();
  InitializeExpansionFallbackPlanner();
}

RefoldEngine::~RefoldEngine() = default;


void RefoldEngine::ResetAttemptStats() {
  lastStats_ = RefoldStats{};
  lastStats_.totalIncludes = model_.GetIncludes().size();
  for (const auto &mi : model_.GetMacroInvocations()) {
    if (!mi.callerMacroId)
      ++lastStats_.totalMacros;
  }
}

void RefoldEngine::EmitTheoremAudit() const {
  const bool satisfied = lastTheoremAudit_.theoremSatisfied;
  const uint64_t unresolvedSelectorWork =
      lastTheoremAudit_.selectorNoSelectable +
      lastTheoremAudit_.selectorUnresolvedCompetitions;
  const uint64_t invalidCarrierCount =
      lastTheoremAudit_.emittedSelectorOnlyExceptionCarriers +
      lastTheoremAudit_.emittedTransitionalTheoremCarriers +
      lastTheoremAudit_.emittedUndischargedCarriers +
      lastTheoremAudit_.emittedUnknownClassCarriers +
      lastTheoremAudit_.emittedOutOfDomainCarriers;
  const uint64_t resolverOpenObligations =
      lastTheoremAudit_.resolverPotentiallyMissingProof +
      lastTheoremAudit_.resolverUnknownDomain +
      lastTheoremAudit_.resolverDeclaredIncompleteKeys +
      lastTheoremAudit_.resolverDeclaredIncompatibleComposition +
      lastTheoremAudit_.resolverDeclaredUnconvertedWitnesses;
  const uint64_t terminalAuditProblems =
      lastTheoremAudit_.nonExplicitTerminalExclusions +
      lastTheoremAudit_.terminalFailureAuditViolations;

  if (satisfied) {
    REFOLD_LOG_INFO("theorem",
         "audit passed: emittedEdits={0} carriers={1} resolverAudits={2} "
         "terminalFailures={3} closureLedgerRows={4}",
         lastTheoremAudit_.emittedNonTerminalEdits,
         lastTheoremAudit_.emittedCarriers,
         lastTheoremAudit_.resolverDomainAudits,
         lastTheoremAudit_.terminalFailureObligations,
         lastTheoremAudit_.resolverClosureLedgerRows);
  } else {
    REFOLD_LOG_WARN("theorem",
         "audit failed: invalidCarriers={0} unresolvedSelectors={1} "
         "resolverOpenObligations={2} terminalAuditProblems={3} "
         "closureLedgerRows={4}",
         invalidCarrierCount, unresolvedSelectorWork,
         resolverOpenObligations, terminalAuditProblems,
         lastTheoremAudit_.resolverClosureLedgerRows);
    if (!lastTheoremAudit_.firstViolation.empty()) {
      REFOLD_LOG_WARN("theorem",
           "first theorem-audit violation: {0}",
           stringutils::showWsWithClip(lastTheoremAudit_.firstViolation,
                                       220));
    }
  }

  REFOLD_LOG_DEBUG("theorem/carriers",
        "emitted carriers: total={0} declared={1} discharged={2} "
        "selectorOnly={3} transitional={4} undischarged={5} "
        "unknownClass={6} outOfDomain={7}",
        lastTheoremAudit_.emittedCarriers,
        lastTheoremAudit_.emittedDeclaredClassCarriers,
        lastTheoremAudit_.emittedDischargedCarriers,
        lastTheoremAudit_.emittedSelectorOnlyExceptionCarriers,
        lastTheoremAudit_.emittedTransitionalTheoremCarriers,
        lastTheoremAudit_.emittedUndischargedCarriers,
        lastTheoremAudit_.emittedUnknownClassCarriers,
        lastTheoremAudit_.emittedOutOfDomainCarriers);
  REFOLD_LOG_DEBUG("theorem/composition",
        "composite edits: total={0} equivalent={1} ordered={2} "
        "uncomposed={3}",
        lastTheoremAudit_.emittedCompositeEdits,
        lastTheoremAudit_.emittedEquivalentCompositeEdits,
        lastTheoremAudit_.emittedOrderedCompositeEdits,
        lastTheoremAudit_.emittedUncomposedCompositeEdits);
  REFOLD_LOG_DEBUG("theorem/selector",
        "selector resolution: competitions={0} resolved={1} "
        "noSelectable={2} unresolved={3} directBypass={4}",
        lastTheoremAudit_.selectorCompetitions,
        lastTheoremAudit_.selectorResolutions,
        lastTheoremAudit_.selectorNoSelectable,
        lastTheoremAudit_.selectorUnresolvedCompetitions,
        lastTheoremAudit_.selectorDirectBypasses);
  REFOLD_LOG_DEBUG("theorem/terminal",
        "terminal fallback audit: explicitExclusions={0} "
        "nonExplicitExclusions={1} primaryFailures={2} secondaryFailures={3} "
        "auditViolations={4}",
        lastTheoremAudit_.explicitTerminalExclusions,
        lastTheoremAudit_.nonExplicitTerminalExclusions,
        lastTheoremAudit_.terminalFailureObligations,
        lastTheoremAudit_.terminalSecondaryFailureObligations,
        lastTheoremAudit_.terminalFailureAuditViolations);
  REFOLD_LOG_DEBUG("theorem/state",
        "state graph: ownerNodes={0} zeroTokenNodes={1} observedComponents={2} "
        "mutatedComponents={3} incomparableNodes={4} missingProducerFacts={5} "
        "directChecks={6} deltaFacts={7} graphEdges={8} gatewayWitnesses={9} "
        "terminalFailures={10} unclosedLocal={11}",
        lastTheoremAudit_.graphOwnerNodes,
        lastTheoremAudit_.graphZeroTokenStateNodes,
        lastTheoremAudit_.graphObservedStateComponents,
        lastTheoremAudit_.graphMutatedStateComponents,
        lastTheoremAudit_.graphIncomparableNodes,
        lastTheoremAudit_.graphMissingProducerFacts,
        lastTheoremAudit_.directStateChecksAudited,
        lastTheoremAudit_.directStateChecksDeltaFacts,
        lastTheoremAudit_.directStateChecksGraphEdges,
        lastTheoremAudit_.directStateChecksGatewayWitnesses,
        lastTheoremAudit_.directStateChecksTerminalFailures,
        lastTheoremAudit_.directStateChecksUnclosedLocal);
  REFOLD_LOG_DEBUG("theorem/resolver",
        "witness resolver: audits={0} declaredInDomain={1} "
        "explicitOutOfDomain={2} ambiguousOutOfDomain={3} "
        "missingProof={4} unknownDomain={5} strictAuthority={6} "
        "strictLegacyFallback={7} strictFailClosed={8} invalidFailClosed={9} "
        "incompleteKeys={10} incompatibleComposition={11} "
        "unconvertedWitnesses={12} closureLedgerRows={13}",
        lastTheoremAudit_.resolverDomainAudits,
        lastTheoremAudit_.resolverDeclaredInDomain,
        lastTheoremAudit_.resolverExplicitOutOfDomain,
        lastTheoremAudit_.resolverAmbiguousOutOfDomain,
        lastTheoremAudit_.resolverPotentiallyMissingProof,
        lastTheoremAudit_.resolverUnknownDomain,
        lastTheoremAudit_.resolverStrictResolverAuthority,
        lastTheoremAudit_.resolverStrictLegacyFallback,
        lastTheoremAudit_.resolverStrictFailClosed,
        lastTheoremAudit_.resolverStrictInvalidFailClosed,
        lastTheoremAudit_.resolverDeclaredIncompleteKeys,
        lastTheoremAudit_.resolverDeclaredIncompatibleComposition,
        lastTheoremAudit_.resolverDeclaredUnconvertedWitnesses,
        lastTheoremAudit_.resolverClosureLedgerRows);
}

void RefoldEngine::EmitRefoldStats() const {
  REFOLD_LOG_INFO("stats",
       "refold summary: expandedIncludes={0}/{1} expandedRootMacros={2}/{3} "
       "terminalFallback={4}",
       lastStats_.expandedIncludes, lastStats_.totalIncludes,
       lastStats_.expandedMacros, lastStats_.totalMacros,
       terminalSink_.HasRequest() ? "yes(raw-B)" : "no");
}


namespace {
// Include-directive source-spelling helpers moved to
// RefoldIncludeMaterializer.cpp with the include-materialization
// routines that use them.

/// True for the narrow punctuation set that may replace a horizontal source
/// gap after lexical-separation proof.
static bool isSeparatorGapReplacementPunctuation(tok::TokenKind kind);
// Macro patch proof helpers that used to live here now belong to
// RefoldMacroPatchPlanner.cpp.  Keep RefoldEngine limited to the
// lexical boundary helper declared above and call shared macro-text
// utilities for cross-subsystem lexer predicates.

} // namespace

/// Construct the LangOptions used by all raw-lexer helper paths.
///
/// The producer records the language spelling that was used to preprocess the
/// TU. Replaying that spelling through CompilerInvocation keeps tokenization
/// decisions, especially literal and comment handling, aligned with the map.
clang::LangOptions RefoldEngine::MakeLexLangOptions(llvm::StringRef langName) {
  DiagnosticOptions diagOpts;
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  auto *client = new IgnoringDiagConsumer();
  DiagnosticsEngine diags(diagIDs, diagOpts, client, /*ShouldOwnClient=*/true);

  auto invocation = std::make_shared<CompilerInvocation>();
  std::string lang = langName.empty() ? "c" : langName.str();
  std::vector<const char *> args = {"-x", lang.c_str()};
  CompilerInvocation::CreateFromArgs(*invocation, ArrayRef<const char *>(args),
                                     diags);
  return invocation->getLangOpts();
}

// ========================== Public entry points ==========================

Expected<std::string> RefoldEngine::Refold(
    const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool noLines, bool strict,
    ProofAuditMode proofAuditMode,
    StringRef finalOutputPath,
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    std::vector<MaterializedEditMapping> *materializedEditMappings,
    FinalLineControlValidationCallback finalLineControlValidationCallback,
    std::vector<SourceGraphOutput> *sourceGraphOutputs) {
  // Build the refold model based on the parsed JSON object.
  auto mOrErr = RefoldModel::FromJson(rootJson);
  if (!mOrErr)
    return mOrErr.takeError();

  // Construct an engine and run the instance pipeline.
  RefoldEngine engine(std::move(*mOrErr), aSource, aToks, aTokOff, bSource,
                      bToks, bTokOff, noLines, strict, proofAuditMode,
                      finalOutputPath, sidebandPragmaEdits,
                      materializedEditMappings,
                      std::move(finalLineControlValidationCallback),
                      sourceGraphOutputs);
  return engine.Refold();
}

std::string RefoldEngine::Refold() {
  if (materializedEditMappings_)
    materializedEditMappings_->clear();
  if (sourceGraphOutputs_)
    sourceGraphOutputs_->clear();
  finalLineControlPruneCandidates_.clear();
  finalLineControlSourceMappings_.clear();

  // The engine is single-pass: it first attempts structural refolding, then
  // (if structural proof discharge requests fallback) resolves the post-
  // structural fallback choice between the proved intermediate expansion stage
  // and the explicit raw-B terminal carrier.
  terminalSink_.Reset();
  ResetAttemptStats();
  TheoremAudit().Reset();

  std::string out = RunSinglePassRefold();

  // Once the structural pass finishes, any surviving theorem-audit violation
  // must be converted into the one explicit terminal fallback rather than
  // merely being reported.
  TheoremAudit().EnforceTheoremAuditInvariants();

  if (terminalSink_.HasRequest()) {
    out = expansionFallbackPlanner_->ResolvePostStructuralFallback();
    finalLineControlPruneCandidates_.clear();
    finalLineControlSourceMappings_.clear();
    if (sourceGraphOutputs_)
      sourceGraphOutputs_->clear();
  }

  // The old final observer/layout scanner is gone.  Producer line-control and
  // builtin-observer facts are therefore no longer lowered into a passive
  // final-stream model here; generation sites have already attached the compact
  // obligation/removal proof records consumed by the pruner below.

  AuditFinalLineControlRemovalProofPopulation(
      finalLineControlPruneCandidates_, "final-line-control-prune-candidates");

  FinalLineControlPruneResult finalLinePrune =
      PruneFinalLineControlDirectives(out, finalLineControlPruneCandidates_,
                                      finalLineControlValidationCallback_);
  AuditFinalLineControlAuthorityContract(finalLinePrune.authority,
                                         "final-line-control-prune");

  if (finalLinePrune.changed) {
    auto mapPointAfterDeletion = [](uint64_t point, uint64_t begin,
                                    uint64_t end) -> uint64_t {
      const uint64_t size = end - begin;
      if (point <= begin)
        return point;
      if (point <= end)
        return begin;
      return point - size;
    };

    for (const FinalLineControlRemovedRange &removedRange :
         finalLinePrune.removedRanges) {
      if (materializedEditMappings_) {
        for (MaterializedEditMapping &mapping : *materializedEditMappings_) {
          mapping.refoldedSourceBegin = mapPointAfterDeletion(
              mapping.refoldedSourceBegin, removedRange.finalBegin,
              removedRange.finalEnd);
          mapping.refoldedSourceEnd = mapPointAfterDeletion(
              mapping.refoldedSourceEnd, removedRange.finalBegin,
              removedRange.finalEnd);
        }
      }

      AdjustFinalLineControlSourceMappingsAfterDeletion(
          finalLineControlSourceMappings_, removedRange.finalBegin,
          removedRange.finalEnd);
    }
  }

  out = finalLinePrune.output;

  EmitRefoldStats();
  EmitTheoremAudit();
  return out;
}

bool RefoldEngine::ValidateSidebandPragmaEditProof(
    const SidebandPragmaEdit &edit, StringRef stage) const {
  // The owner occurrence, source range, B byte envelope, and payload are the
  // emitted edit facts classified by OwnerLocalSourceEditProof.  Delegate the
  // shared validation/reporting gate to the sideband proof service so the TU
  // and include-owned paths cannot drift in their terminal-fallback policy.
  return validateAndReportSidebandPragmaEditProof(
      edit, static_cast<uint64_t>(bSource_.size()), terminalSink_, stage,
      /*traceSuccess=*/true);
}

bool RefoldEngine::AppendSidebandPragmaSourceEdits(
    StringRef tuPath, StringRef tuBytes,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  if (sidebandPragmaEdits_.empty())
    return true;

  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits_) {
    if (!ValidateSidebandPragmaEditProof(sideband, "pragma/sideband"))
      return false;

    const auto sourceRange = sideband.SourceByteRange();

    // The current clang-refold artifact is one emitted TU source file.  A
    // sideband pragma whose source location is inside a header must be handled
    // by an include/materialization proof; applying it blindly to the TU would
    // edit the wrong owner.  Reject that class explicitly instead of silently
    // preserving or dropping a header pragma.
    if (!pathIdentity_.PathsEqual(sideband.SourcePath(), tuPath)) {
      if (!sideband.HasConcreteIncludeOwner()) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
          TerminalFallbackObligationKind::PragmaBoundaryKnown,
          TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary),
            "pragma/sideband",
            llvm::formatv(
                "sideband pragma edit targets non-TU owner path='{0}' "
                "site=[{1},{2}) without a unique include owner",
                sideband.SourcePath(), sourceRange.first, sourceRange.second)
                .str());
        return false;
      }

      continue;
    }

    if (!sideband.SourceIsWithinOwnerBytes(
            static_cast<uint64_t>(tuBytes.size()))) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::PragmaBoundaryKnown,
              TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary),
          "pragma/sideband",
          llvm::formatv(
              "sideband pragma edit has invalid TU range site=[{0},{1}) "
              "tuSize={2}",
              sourceRange.first, sourceRange.second, tuBytes.size())
              .str());
      return false;
    }

    // Sideband pragmas are zero-normal-token artifacts: their raw directive
    // text appeared in the `.i` replay surface, but the producer deliberately
    // did not count that directive text as ordinary PP tokens.  Once the driver
    // removes the sideband directive tokens from A/B before diffing, the source
    // pragma itself still needs an explicit source edit so preserved comments
    // and nearby code can stay on the normal structural path.
    //
    // Apply the same line-state repair used for ordinary TU byte edits.  A
    // sideband block replacement can change the number of physical directive
    // lines before preserved TU suffix bytes; in --with-lines mode the suffix
    // must resume at its original logical TU line instead of drifting with the
    // replacement's physical line count.  The materialized edit-map range still
    // describes only the B-side sideband payload, not the synthetic #line
    // directive that may be appended for resynchronization.
    ResyncOutcome ro = textEditAssembler_->ApplyResyncOrPend(
        tuBytes, sourceRange.first, sourceRange.second,
        sideband.ReplacementText(), tuPath);
    TextEdit edit{sourceRange.first,
                  sourceRange.second,
                  std::move(ro.text),
                  std::move(ro.pending),
                  std::nullopt,
                  {},
                  {},
                  {}};
    edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);
    textEditAssembler_->StampTextEditMaterializedBReplayProof(edit, sideband);
    textEditAssembler_->AttachAcceptedResultCarrier(
        edit,
        ProofLattice().BuildAcceptedTUTextEditCandidate(
            AcceptedPathKind::TUByteSpanConservativeEdit, sourceRange.first,
            sourceRange.second, sideband.ReplacementText()));
    structuralHunkDispatcher.AddTUEdit(std::move(edit));
  }

  return true;
}

bool RefoldEngine::MaybeConsumeOrdinarySeparatorGapForPunctuation(
    StringRef tuPath, StringRef tuBytes, std::pair<uint64_t, uint64_t> &span,
    StringRef replacement, StringRef tracePrefix) const {
  if (span.first != span.second || replacement.empty() ||
      stringutils::isWs(replacement.front()) || span.first == 0 ||
      span.first >= tuBytes.size() ||
      (tuBytes[span.first - 1] != ' ' && tuBytes[span.first - 1] != '\t') ||
      stringutils::isWs(tuBytes[span.first]))
    return false;

  auto intervalOverlapsSpelledArtifact = [&](uint64_t begin,
                                             uint64_t end) -> bool {
    for (const auto &inc : model_.GetIncludes()) {
      if (pathIdentity_.PathsEqual(inc.sitePath, tuPath) && inc.siteB < end &&
          begin < inc.siteE)
        return true;
    }
    for (const auto &m : model_.GetMacroInvocations()) {
      if (m.invFile && !m.invFile->empty() && !pathIdentity_.PathsEqual(*m.invFile, tuPath))
        continue;
      if (m.invB && m.invE && *m.invB < end && begin < *m.invE)
        return true;
    }
    return false;
  };

  uint64_t gapBegin = span.first;
  while (gapBegin > 0 &&
         (tuBytes[gapBegin - 1] == ' ' || tuBytes[gapBegin - 1] == '\t'))
    --gapBegin;

  if (gapBegin >= span.first ||
      intervalOverlapsSpelledArtifact(gapBegin, span.first))
    return false;

  std::optional<RefoldLexBoundaryToken> leftTok =
      refoldLastLexToken(tuBytes.take_front(gapBegin), lexLang_);
  std::optional<RefoldLexBoundaryToken> rightTok =
      refoldFirstLexToken(tuBytes.drop_front(span.first), lexLang_);
  std::optional<RefoldLexBoundaryToken> replFirstTok =
      refoldFirstLexToken(replacement, lexLang_);
  std::optional<RefoldLexBoundaryToken> replLastTok =
      refoldLastLexToken(replacement, lexLang_);

  if (!leftTok || !rightTok || !replFirstTok || !replLastTok ||
      leftTok->End != gapBegin || rightTok->Begin != 0 ||
      !isSeparatorGapReplacementPunctuation(replFirstTok->Kind) ||
      refoldNeedsLexicalSeparator(*leftTok, *replFirstTok, lexLang_) ||
      refoldNeedsLexicalSeparator(*replLastTok, *rightTok, lexLang_))
    return false;

  span.first = gapBegin;
  return true;
}



bool RefoldEngine::TUInsertionBeforeMaterializedInclude(
    const diffutils::Hunk &h, StringRef tuPath,
    const std::pair<uint64_t, uint64_t> &span,
    bool requireVisibleReplayText) const {
  if (!h.isInsertOnly() || span.first != span.second)
    return false;
  if (!TUEditPlanner().AnchorToExactSlotBoundaryFromPPGap(tuPath, h.aStart))
    return false;

  const uint64_t maxPP = model_.GetTokensCountA();
  std::optional<uint64_t> leftInc =
      (h.aStart > 0) ? model_.InnermostIncludeAtPP(h.aStart - 1) : std::nullopt;
  std::optional<uint64_t> rightInc =
      h.aStart < maxPP ? model_.InnermostIncludeAtPP(h.aStart) : std::nullopt;
  if (leftInc || !rightInc)
    return false;

  return llvm::any_of(
      sidebandPragmaEdits_, [&](const SidebandPragmaEdit &sideband) {
        return sideband.TargetsInclude(*rightInc) &&
               (!requireVisibleReplayText || sideband.EmitsVisibleReplayText());
      });
}

TextEdit RefoldEngine::BuildDirectTUHunkTextEdit(
    const diffutils::Hunk &h, uint64_t hunkIndex,
    const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
    StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
    std::optional<uint64_t> materializedBByteBegin,
    std::optional<uint64_t> materializedBByteEnd,
    AcceptedPathKind acceptedPath) const {
  DirectTUHunkEditPlan plan = TUEditPlanner().BuildDirectTUHunkEditPlan(
      h, hunkIndex, span, std::move(resync), acceptedPayload, rawTUStart,
      rawTUEnd, materializedBByteBegin, materializedBByteEnd, acceptedPath);

  // The TU edit planner now owns the direct-hunk planning record.  This wrapper
  // remains the compatibility bridge to the existing TextEdit/audit boundary:
  // it performs the same carrier stamping as the pre-10D implementation while
  // avoiding any change to final edit ordering or emitted text.
  assert(plan.Resync && "direct TU hunk edit plan requires resync payload");
  auto spanBytes = plan.Span.byteRange();
  TextEdit edit{spanBytes.first,
                spanBytes.second,
                std::move(plan.Resync->text),
                std::move(plan.Resync->pending),
                std::nullopt,
                {},
                {},
                {}};
  edit.lineControlPruneCandidates =
      std::move(plan.Resync->lineControlPruneCandidates);
  edit.isDirectTUHunkEdit = true;
  edit.directTUHunkIndex = plan.HunkIndex;
  edit.directTUHunkAStart = plan.Hunk.aStart;
  edit.directTUHunkAEnd = plan.Hunk.aEnd;
  edit.directTUHunkBStart = plan.Hunk.bStart;
  edit.directTUHunkBEnd = plan.Hunk.bEnd;
  edit.directTURawStart = plan.RawTUStart;
  edit.directTURawEnd = plan.RawTUEnd;
  edit.directTUFinalStart = spanBytes.first;
  edit.directTUFinalEnd = spanBytes.second;
  if (plan.MaterializedBByteBegin && plan.MaterializedBByteEnd)
    textEditAssembler_->StampTextEditMaterializedBByteRange(
        edit, *plan.MaterializedBByteBegin, *plan.MaterializedBByteEnd);
  AcceptedResultCandidate candidate =
      ProofLattice().BuildAcceptedTUTextEditCandidate(
          plan.AcceptedPath, spanBytes.first, spanBytes.second,
          plan.AcceptedPayload);
  ProofLattice().AttachMixedOwnerTilingWitnessForTokenEnvelope(
      candidate.proofSummary, plan.Hunk.aStart, plan.Hunk.aEnd,
      plan.Hunk.bStart, plan.Hunk.bEnd);
  ProofLattice().AttachLineControlObserverWitness(candidate);
  ProofLattice().AttachCounterStateWitness(candidate);
  ProofLattice().RefreshAcceptedCandidateEmissionPathInventory(candidate);
  textEditAssembler_->AttachAcceptedResultCarrier(edit, candidate);
  return edit;
}

std::string RefoldEngine::RunSinglePassRefold() {
  // Make sure that when we re-lex the A-stream tokens that it matches the token
  // count as listed in the refold map JSON file.
  if (static_cast<size_t>(model_.GetTokensCountA()) != aToks_.size()) {
    // A-side token-count disagreement means the refold map and the supplied
    // `--pp` replay file do not describe the same token stream.  One known
    // source of this shape is a producer that prints sideband pragma directive
    // text in the `.i` file while recording only the tokens produced after the
    // pragma has affected PP state.  There is no deterministic structural proof
    // we can discharge from that inconsistent A surface, so do not abort the
    // process.  Escape to the explicit terminal carrier (`--pp-mod`) instead.
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::ProducerFactsAvailable,
            TerminalFallbackFailureReason::MissingProducerFacts,
            TerminalFallbackFailureContext::ForStateComponent(
                "AStreamTokenCount")),
        "tok",
        llvm::formatv(
            "A-stream token count mismatch: model reported {0} tokens, but "
            "lexed sequence (aToks) has {1} tokens",
            model_.GetTokensCountA(), aToks_.size())
            .str());
    return std::string();
  }

  StringRef tuPath = model_.GetSourcePath();

  REFOLD_LOG_INFO("plan",
       "starting refold: tu={0} ppBytes={1} ppModBytes={2} ppTokens={3} "
       "ppModTokens={4}",
       tuPath, aSource_.size(), bSource_.size(), aToks_.size(), bToks_.size());

  abTokHunks_.clear();
  abTokMapA2B_.clear();
  abTokMapB2A_.clear();

  // Read in the translation unit file / C source.
  std::unique_ptr<llvm::MemoryBuffer> tuBuffer;
  {
    const auto fullTuPath = lineDirs_.ToAbsolutePath(tuPath);
    auto bufOrErr = MemoryBuffer::getFile(fullTuPath);
    if (!bufOrErr) {
      // Fatal and stop: unreachable past this point.
      REFOLD_LOG_FATAL("src/load", "failed to read C source: {0} ({1})", fullTuPath,
            bufOrErr.getError().message());
    }

    // Don't need a copy of the bytes here due to lifetime reasoning.
    tuBuffer = std::move(*bufOrErr);
  }
  StringRef tuBytes = tuBuffer->getBuffer();

  // 1) Build the initial A/B token diff and refresh the per-run diff caches.
  //
  // RefoldTokenDiffPlanner owns lexeme mapping, LCS provenance construction,
  // normalized token-hunk derivation, A/B token map population, and raw
  // byte-hunk cache construction.  Owner-aware tiling starts from this
  // deterministic token-diff plan below.
  assert(tokenDiffPlanner_ && "token diff planner service not initialized");
  RefoldTokenDiffPlanner::TokenDiffPlan diffPlan = tokenDiffPlanner_->Plan();
  std::vector<diffutils::Hunk> hunks = std::move(diffPlan.Hunks);

  REFOLD_LOG_DEBUG("plan",
        "refold model loaded: tu={0} includes={1} macroInvocations={2} "
        "ppTokenMapEntries={3}",
        tuPath, model_.GetIncludes().size(),
        model_.GetMacroInvocations().size(), model_.GetTokmapByPP().size());

  // The token-diff planner caches the pre-tiling token hunks for A->B envelope
  // projection.  Mixed-owner tiling may replace this vector with split hunks
  // after it proves a deterministic owner partition.

  assert(mixedOwnerTilingPlanner_ &&
         "mixed-owner tiling planner service not initialized");
  RefoldMixedOwnerTilingPlanner::MixedOwnerTilingPlan mixedOwnerPlan =
      mixedOwnerTilingPlanner_->Plan(std::move(hunks), tuBytes);
  hunks = std::move(mixedOwnerPlan.Hunks);

  if (inDebugMode()) {
    size_t insertOnlyHunks = 0;
    size_t deleteOnlyHunks = 0;
    size_t replaceHunks = 0;
    for (const auto &h : hunks) {
      if (h.isInsertOnly()) {
        ++insertOnlyHunks;
      } else if (h.bStart >= h.bEnd) {
        ++deleteOnlyHunks;
      } else {
        ++replaceHunks;
      }
    }
    debug("diff",
          "token diff normalized: hunks={0} replacements={1} insertions={2} "
          "deletions={3} mixedOwnerWitnesses={4}",
          hunks.size(), replaceHunks, insertOnlyHunks, deleteOnlyHunks,
          mixedOwnerTilingWitnesses_.size());
  }

  // Build provenance for token-level pure insertions (B-only hunks) and
  // pre-claim standalone insertions before macro patching so whole-cover
  // replacements can deterministically avoid double-emitting insertion
  // payloads.
  BInsertionLedger().BuildProvenance(hunks);
  BInsertionLedger().PreclaimStandaloneInsertions(tuPath, hunks);

  // Validate B-token envelopes for every hunk. In trace mode, also show the
  // exact B-side fragment that later owner/macro/include proofs must explain.
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // Case A: The hunk is logically empty (e.g., a pure deletion)
    if (h.bStart >= h.bEnd) {
      continue;
    }

    // Case B: Hunk indices are out of bounds for the token-to-byte map
    if (h.bEnd >= bTokOff_.size()) {
      REFOLD_LOG_FATAL("hunks",
            "#{0} {1:verbose} B=OUT-OF-BOUNDS: h.bEnd={2} map.size={3}", i, h,
            h.bEnd, bTokOff_.size());
      continue;
    }

    const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];
    const size_t b1 = bTokOff_[static_cast<size_t>(h.bEnd)];

    // Case C: The token-to-byte map contains sentinels (virtual/synthetic
    // tokens)
    if (b0 == StringRef::npos || b1 == StringRef::npos) {
      REFOLD_LOG_FATAL("hunks", "#{0} {1:verbose} B=SENTINEL: b0={2} b1={3}", i, h,
            (b0 == StringRef::npos ? "npos" : "valid"),
            (b1 == StringRef::npos ? "npos" : "valid"));
      continue;
    }

    // Case D: Byte offsets are inverted (corrupt map or out-of-order tokens)
    if (b1 < b0) {
      REFOLD_LOG_FATAL("hunks", "#{0} {1:verbose} B=INVERTED-OFFSETS: b0={2} b1={3}", i, h,
            b0, b1);
      continue;
    }

    // Final physical safety clamp (prevents crashes if map is stale relative to
    // source)
    const size_t lo = std::min(b0, bSource_.size());
    const size_t hi = std::min(b1, bSource_.size());

    if (inTraceMode()) {
      StringRef bfrag = bSource_.substr(lo, hi - lo);
      trace("diff/hunk",
            "hunk #{0}: aTokens=[{1},{2}) bTokens=[{3},{4}) bBytes=[{5},{6}) "
            "bText=\"{7}\"",
            i, h.aStart, h.aEnd, h.bStart, h.bEnd, lo, hi,
            stringutils::showWsWithClip(bfrag, 160));
    }
  }

  // 4) Classify hunks and collect per-target edits.  The dispatcher owns the
  // mutable staging state for TU edits, include-local patches, and macro patch
  // merge buckets; this function still makes the proof/owner decisions until
  // the remaining structural dispatch policy is extracted.
  RefoldStructuralHunkDispatcher structuralHunkDispatcher;
  std::vector<TextEdit> &tuEdits =
      structuralHunkDispatcher.MutableTUEditsForRepairAndEmission();
  if (!AppendSidebandPragmaSourceEdits(tuPath, tuBytes,
                                       structuralHunkDispatcher))
    return std::string();

  // Diagnostic-only helper: derive the B-token envelope that corresponds to an
  // A-token interval by looking only at the final A->B token map.
  //
  // This is used to compare two independent views of the same hunk:
  //
  //   1. the B envelope selected from byte-hunk provenance, and
  //   2. the B envelope implied by token-level A->B matches.
  //
  // The returned interval is therefore for logging/auditing envelope drift
  // only. It must not become a semantic proof source, because unmatched edit
  // interiors may require approximation from neighboring mapped tokens.
  auto hasTopLevelCommaInReplacement = [&](StringRef text) -> bool {
    return refoldMacroActualHasTopLevelComma(text, lexLang_);
  };

  auto theoremLatticePrefersExactTUArgEditOverMacroArgsOnly =
      [&](const RefoldModel::MacroInvocation &macro,
          const diffutils::Hunk &hunk,
          const MacroPatch &macroCandidate) -> bool {
    // Cross-domain theorem-lattice tie-breaker:
    //
    // The same PP hunk can sometimes be represented in two valid ways:
    //
    //   1. a macro args-only rewrite, e.g. ID(/*keep*/ 1) -> ID(2)
    //   2. an exact TU byte edit inside the original argument spelling,
    //      e.g. ID(/*keep*/ 1) -> ID(/*keep*/ 2)
    //
    // The named tie-breaker may prefer the TU edit when it is provably
    // equivalent, because it preserves callsite trivia that macro argument
    // reconstruction does not retain. This is intentionally not a general "TU
    // beats macro" rule; it applies only to a single direct argument occurrence
    // whose source spelling can be edited without changing macro arity or
    // expansion multiplicity.
    //
    // Only compete against an already-proven, structure-preserving args-only
    // macro candidate. Other macro proofs have their own stronger invariants
    // and should not be displaced by this narrow preference.
    if (macroCandidate.proof.kind != MacroPatchProofKind::ArgsOnlyStandard ||
        !macroCandidate.proof.preservesInvocationStructure)
      return false;

    const AcceptedResultCandidate macroAccepted =
        ProofLattice().BuildAcceptedMacroCandidate(macroCandidate);
    if (!ProofLattice().IsSelectableAcceptedResultCandidate(macroAccepted))
      return false;

    // This preference rewrites existing source bytes inside an argument. Pure
    // insertions/deletions and empty B ranges need different anchoring rules.
    if (!hunk.isReplace() || hunk.aStart >= hunk.aEnd ||
        hunk.bStart >= hunk.bEnd)
      return false;

    // The invocation itself must be spelled in the main TU. If the invocation
    // is include-owned or lacks callsite provenance, rewriting its argument
    // bytes here would cross source ownership boundaries.
    if (!macro.invB || !macro.invE || !macro.invFile ||
        !pathIdentity_.PathsEqual(*macro.invFile, tuPath))
      return false;

    // The PP hunk must map back to a concrete TU token range. This prevents
    // choosing a TU-byte edit for tokens that only exist through macro body
    // spelling, include materialization, or another non-TU source.
    if (!OwnerClassifier().HunkMapsToTU(hunk.aStart, hunk.aEnd, tuPath))
      return false;

    auto spanPlan =
        TUEditPlanner().PlanTUByteSpan(hunk.aStart, hunk.aEnd, tuPath);
    if (!spanPlan || spanPlan->TUByteBegin >= spanPlan->TUByteEnd)
      return false;
    auto span = spanPlan->byteRange();

    // The concrete source bytes for the hunk must lie inside the invocation
    // callsite. Otherwise the edit is not an argument-local alternative to the
    // macro candidate.
    if (span.first < *macro.invB || span.second > *macro.invE)
      return false;

    std::optional<uint32_t> touchedArgIdx;
    for (const auto &occ : macro.argSpans) {
      if (occ.kind != PPArgSpanKind::Standard)
        continue;

      // Find the single STANDARD expansion occurrence that covers the PP hunk.
      // If more than one argument occurrence covers it, the hunk is ambiguous
      // and must stay on the existing macro proof path.
      if (occ.begin <= hunk.aStart && hunk.aEnd <= occ.end) {
        if (touchedArgIdx)
          return false;
        touchedArgIdx = occ.argIdx;
      }
    }
    if (!touchedArgIdx)
      return false;

    unsigned directStandardOccurrences = 0;
    for (const auto &occ : macro.argSpans) {
      if (occ.argIdx != *touchedArgIdx)
        continue;

      // Editing the argument source changes every expansion of that formal in
      // this invocation. Therefore the preference is sound only when the formal
      // appears exactly once, and only as a direct STANDARD substitution.
      if (occ.kind != PPArgSpanKind::Standard)
        return false;
      ++directStandardOccurrences;
    }

    // Stringify and paste uses transform the argument spelling before it
    // reaches the PP output. A direct TU argument edit is not equivalent to an
    // args-only reconstruction for those cases.
    for (const auto &occ : macro.stringifySpans) {
      if (occ.argIdx == *touchedArgIdx)
        return false;
    }
    for (const auto &occ : macro.pasteSpans) {
      if (occ.argIdx == *touchedArgIdx)
        return false;
    }

    if (directStandardOccurrences != 1)
      return false;

    if (*touchedArgIdx >= macro.invArgRanges.size())
      return false;
    const auto &argRange = macro.invArgRanges[*touchedArgIdx];
    if (!argRange.first || !argRange.second ||
        *argRange.second < *argRange.first) {
      return false;
    }

    // The source byte span must be contained in the original argument spelling,
    // not merely somewhere inside the invocation parentheses.
    if (span.first < *argRange.first || span.second > *argRange.second)
      return false;

    StringRef replacement = refoldSliceExactTokenCoverage(
        bTokOff_, bToks_, bSource_, hunk.bStart, hunk.bEnd);

    // Replacing an argument subrange with a top-level comma would split the
    // original invocation argument list. That is a semantic arity change, so it
    // must remain on the macro reconstruction/fallback path.
    if (hasTopLevelCommaInReplacement(replacement))
      return false;

    // This preference is only a tie-breaker for the case where the macro patch
    // and the direct source edit spell the same invocation.  Empty-slot and
    // __VA_OPT__ repairs can move tokens between formals while still preserving
    // the macro call; deferring those patches to a literal TU argument edit
    // produces a validating but less structural result such as
    // `MAYBE_PLUS(3 + 4)` instead of `MAYBE_PLUS(3, 4)`.  Project the TU hunk
    // back into the original invocation spelling and require byte-equivalence
    // with the accepted macro patch before letting the TU edit compete.  If the
    // two spellings differ, the macro proof is carrying structure that the TU
    // byte edit would erase, so the TU edit is not a harmless substitute.
    if (!macro.invText || !macro.invB || span.second < span.first ||
        span.first < *macro.invB) {
      return false;
    }
    const uint64_t relBegin64 = span.first - *macro.invB;
    const uint64_t relEnd64 = span.second - *macro.invB;
    if (relEnd64 < relBegin64 || relEnd64 > macro.invText->size())
      return false;
    std::string directEditInvocation = stringutils::replaceRange(
        macro.invText->str(), static_cast<size_t>(relBegin64),
        static_cast<size_t>(relEnd64), replacement.str());
    if (StringRef(directEditInvocation).trim() !=
        StringRef(macroCandidate.replacement).trim()) {
      return false;
    }

    AcceptedResultCandidate tuCandidate = ProofLattice().BuildAcceptedTUTextEditCandidate(
        AcceptedPathKind::TUByteSpanMappedEdit, span.first, span.second,
        replacement);
    tuCandidate.proofSummary.selectionTieBreaker =
        TheoremSelectionTieBreakerKind::
            ExactTUArgumentEditOverEquivalentMacroArgsOnly;

    const bool latticePrefersTU =
        ProofLattice().IsSelectableAcceptedResultCandidate(tuCandidate) &&
        ProofLattice().LatticePrefers(tuCandidate.proofSummary, macroAccepted.proofSummary) &&
        !ProofLattice().LatticePrefers(macroAccepted.proofSummary, tuCandidate.proofSummary);
    if (!latticePrefersTU)
      return false;

    return true;
  };

  // Iterate over all hunks:
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // These shape predicates are semantic control-flow inputs, not trace
    // payload. Keep them even though the old hunk-shape log is gone.
    const bool isIns = h.isInsertOnly();
    const bool isDel = h.isDeleteOnly();

    // a) Segment-aware owner classification: this decides TU vs include vs “no
    // segment”.
    Owner owner = OwnerClassifier().ClassifyOwnerWithSegments(tuPath, h);

    // b) Macro call-site still has priority over TU/include.  Half-open
    // boundary insertions are not macro-owned by default; macro-domain boundary
    // selectors expose only the narrow insertion-at-cover-edge candidates that
    // a later whole-cover/definition-tape proof must still validate before the
    // hunk is absorbed into a callsite rewrite.
    const RefoldModel::MacroInvocation *macroTarget =
        macroTopology_.SmallestCoveringPatchableMacro(h.aStart, h.aEnd,
                                                      owner.includeId);
    if (!macroTarget && isIns)
      macroTarget =
          macroBoundarySelector_.RightBoundaryVaOptActivationMacro(
              h.aStart, owner.includeId);
    if (!macroTarget && isIns)
      macroTarget =
          macroBoundarySelector_.BoundaryGeneratedSelectorMacro(
              h.aStart, owner.includeId);
    if (!macroTarget && isIns)
      macroTarget =
          macroBoundarySelector_.BoundaryDefinitionTapeReplayMacro(
              h, owner.includeId);
    if (auto *m = macroTarget) {
      if (m->invB && m->invE) {
        bool appliedMacroPatch = false;
        for (const RefoldModel::MacroInvocation *target = m;
             target != nullptr;) {
          RefoldStructuralHunkDispatcher::MacroPatchStagingSlot stagingSlot =
              structuralHunkDispatcher.PrepareMacroPatchStagingSlot(*target);
          RefoldMacroPatchPlanner::ExistingMacroPatchContext
              existingPatchContext;
          existingPatchContext.Patch = stagingSlot.ExistingPatch;
          existingPatchContext.IsCallsite = stagingSlot.ExistingIsCallsite;

          // Try to build a whole-cover replacement at the current target
          // invocation. If successful, install/replace the callsite patch for
          // this macro id and stop climbing the caller chain.
          auto updated = MacroPatchPlanner().BuildMacroInvocationPatchWholeCover(
              *target, h, stagingSlot.CurrentInvocationText,
              structuralHunkDispatcher.MacroPatchMergeBucketsForPlanner(),
              existingPatchContext);
          if (updated) {
            if (!stagingSlot.ExistingPatch &&
                theoremLatticePrefersExactTUArgEditOverMacroArgsOnly(
                    *target, h, *updated)) {
              break;
            }

            const Owner currentPatchOwner =
                MacroPatchPlanner().NormalizeHunkOwnerForPatch(tuPath, h);
            if (stagingSlot.ExistingPatch)
              MacroPatchPlanner().CarryMacroPatchOwnerCertificate(
                  *updated, *stagingSlot.ExistingPatch);
            structuralHunkDispatcher.MergeMaterializedBTokenRangeFromSlot(
                *updated, stagingSlot, h);
            MacroPatchPlanner().StampMacroPatchOwnerWitness(
                *updated, currentPatchOwner);
            updated->macroId = stagingSlot.PatchKey;
            structuralHunkDispatcher.StageMacroPatch(stagingSlot,
                                                     std::move(*updated));
            appliedMacroPatch = true;
            break;
          }

          // No deterministic whole-cover patch at this child invocation.
          // Climb to the immediate caller macro and retry at that enclosing
          // callsite, so the edit can be represented at a higher macro layer
          // if needed.
          if (!target->callerMacroId)
            break;
          const RefoldModel::MacroInvocation *parent =
              macroTopology_.FindMacroInvocationById(*target->callerMacroId);
          if (!parent)
            break;
          target = parent;
        }

        if (appliedMacroPatch)
          continue;
      }
    }

    // c) Special case: pure insertions exactly at the boundary between sibling
    // includes that share a common parent. In this case, per policy, the
    // insertion should be attached to the *parent* include so that the
    // refolded C places it between `#include` lines, not inside any child.
    if (isIns) {
      const RefoldModel::IncludeItem *parentBoundaryInc = nullptr;
      if (auto boundaryPlan =
              TUEditPlanner().FindBoundaryParentIncludeForPureInsertion(h))
        parentBoundaryInc = model_.GetIncludeById(boundaryPlan->IncludeId);
      if (parentBoundaryInc) {
        IncludePatch patch =
            IncludeInsertionPlanner().BuildIncludeInsertionPatch(
                *parentBoundaryInc, h);
        patch.ownerHasCondArmCert = owner.condArmId.has_value();
        patch.ownerCondArmIdCert = owner.condArmId.value_or(0);
        structuralHunkDispatcher.AddIncludePatch(parentBoundaryInc,
                                                 std::move(patch));
        continue;
      }

      if (owner.kind == OwnerKind::Include && owner.includeId) {
        std::optional<uint64_t> firstCond =
            model_.FirstConditionalArmStartA(*owner.includeId);
        if (firstCond && h.aStart <= *firstCond) {
          const RefoldModel::IncludeItem *inc =
              model_.GetIncludeById(*owner.includeId);
          // NOTE: `inc` cannot be null if owner has an `includeId`
          IncludePatch patch =
              IncludeInsertionPlanner().BuildIncludeInsertionPatch(*inc, h);
          patch.ownerHasCondArmCert = owner.condArmId.has_value();
          patch.ownerCondArmIdCert = owner.condArmId.value_or(0);
          structuralHunkDispatcher.AddIncludePatch(inc, std::move(patch));
          continue;
        }
      }
    }


    // d) Include-owned edit (segment policy already enforced in
    // RefoldOwnerClassifier).
    if (owner.kind == OwnerKind::Include && owner.includeId) {
      const RefoldModel::IncludeItem *inc =
          model_.GetIncludeById(*owner.includeId);
      // NOTE: `inc` cannot be null if owner has an `includeId`
      IncludePatch patch =
          IncludeInsertionPlanner().BuildIncludeInsertionPatch(*inc, h);
      patch.ownerHasCondArmCert = owner.condArmId.has_value();
      patch.ownerCondArmIdCert = owner.condArmId.value_or(0);
      structuralHunkDispatcher.AddIncludePatch(inc, std::move(patch));
      continue;
    }

    // e) TU edit? (segments + existing TU mapping cooperate here).
    // We rely on the token→file map as the source of truth for TU ownership.
    // Segment classification is only used to detect include-owned edits; it
    // should not force a hunk into the TU if any mapped token belongs to a
    // header. So only treat it as TU when the hunk map says so.
    bool mapsToTU = OwnerClassifier().HunkMapsToTU(h.aStart, h.aEnd, tuPath);

    if (owner.kind == OwnerKind::TU && !mapsToTU) {
      // Deterministic rule: TU ownership must be supported by provenance. If
      // tokmap-based evidence does not indicate TU ownership, do not force TU
      // edits (even for insertions). Leave owner unresolved so strict mode can
      // surface the deficiency.
      owner = Owner::Unknown();
    }

    if (mapsToTU) {
      auto spanPlan =
          TUEditPlanner().PlanTUByteSpan(h.aStart, h.aEnd, tuPath); // [b,e)
      if (spanPlan) {
        auto span = spanPlan->byteRange();

        std::string repl;
        const uint64_t rawTUStart = span.first;
        const uint64_t rawTUEnd = span.second;
        std::optional<uint64_t> materializedBByteBegin;
        std::optional<uint64_t> materializedBByteEnd;
        if (auto bBytes =
                sourceMapper_.BTokenRangeToByteRange(h.bStart, h.bEnd)) {
          materializedBByteBegin = bBytes->first;
          materializedBByteEnd = bBytes->second;
        }

        if (h.bStart < h.bEnd) {
          StringRef bSlice =
              h.isInsertOnly()
                  ? refoldSliceTokenEnvelope(bTokOff_, bSource_, h.bStart,
                                             h.bEnd)
                  : refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                                  h.bStart, h.bEnd);
          repl.assign(bSlice.data(), bSlice.data() + bSlice.size());
          if (h.isInsertOnly()) {
            // Pure insertions use the token envelope, which may include
            // zero-normal-token sideband directive lines adjacent to the
            // ordinary inserted tokens.  If a sideband replacement/deletion is
            // also proved and materialized through its owning include/TU atom,
            // the ordinary insertion must not replay the same B bytes.
            std::optional<uint64_t> envelopeBegin;
            std::optional<uint64_t> envelopeEnd;
            if (!bSlice.empty()) {
              envelopeBegin =
                  static_cast<uint64_t>(bSlice.data() - bSource_.data());
              envelopeEnd =
                  *envelopeBegin + static_cast<uint64_t>(bSlice.size());
            }
            repl = textEditAssembler_->StripSeparatelyOwnedSidebandReplay(
                repl, envelopeBegin, envelopeEnd);
          }
          const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];

          // Token-envelope byte ranges begin at the first inserted token, so
          // they do not include any spaces or tabs that appear immediately
          // before that token in B on the same line. For a zero-width TU
          // insertion, preserve those preceding spaces or tabs when forming
          // the inserted text, unless equivalent spacing is already present
          // immediately to the left of the insertion point in the TU.
          if (span.first == span.second && h.bStart > 0) {
            size_t p = b0;
            while (p > 0) {
              char c = bSource_[p - 1];
              if (c == ' ' || c == '\t') {
                --p;
                continue;
              }
              break;
            }
            if (p < b0) {
              bool hasSpaceLeft =
                  (span.first > 0 &&
                   (tuBytes[span.first - 1] == ' ' ||
                    tuBytes[span.first - 1] == '\t'));
              if (!hasSpaceLeft) {
                repl.insert(0, std::string(bSource_.data() + p, b0 - p));
                materializedBByteBegin = static_cast<uint64_t>(p);
              }
            }
          }
        }

        bool consumedSeparatorGapForPunctuation = false;

        if (h.isInsertOnly()) {
          consumedSeparatorGapForPunctuation =
              MaybeConsumeOrdinarySeparatorGapForPunctuation(
                  tuPath, tuBytes, span, StringRef(repl), "TU");
        }

        TUEditPlanner().MaybeExtendTUSpanOverClosedTrailingCallSuffix(
            h, tuPath, tuBytes, repl, span);

        bool advancedOverSourceLineControlPrefix =
            MaybeAdvanceTUInsertionPastSourceLineControlPrefix(
                h, tuPath, tuBytes, span, "TU");

        // Is this span replacing a TU "gap" (bytes that are all whitespace)?
        std::string original;
        if (span.second > span.first) {
          original.assign(tuBytes.data() + span.first,
                          tuBytes.data() + span.second);
        } else if (span.second < span.first) {
          REFOLD_LOG_FATAL("tu/span", "invalid TU byte span: [{0},{1})",
                           span.first, span.second);
        }

        bool replacingGap =
            !original.empty() && stringutils::isWs(original);

        // If we’re replacing a non-empty TU gap and the inserted text doesn’t
        // start with WS, prefix EXACTLY ONE space from the gap to preserve
        // “return injected” (no double spaces).
        if (replacingGap && !consumedSeparatorGapForPunctuation &&
            !repl.empty() && !stringutils::isWs(repl.front())) {
          repl.insert(repl.begin(), ' ');
        }

        // Final boundary spacing fixup:
        // - On the left, only let PadAtBoundaries add a space if we did not
        //   already preserve whitespace from a replaced TU gap; otherwise we
        //   could duplicate spacing.
        // - On the right, always allow padding if the replacement would
        //   otherwise glue to the following TU text.
        // Keep a copy for logging; PadAtBoundaries consumes via move.
        std::string rawRepl = repl;

        std::string padded =
            PadAtBoundaries(tuBytes, static_cast<size_t>(span.first),
                            static_cast<size_t>(span.second), std::move(repl),
                            /*allowLeft*/ !replacingGap,
                            /*allowRight*/ true);

        const bool skipLocalResync =
            TUInsertionBeforeMaterializedInclude(
                h, tuPath, span, /*requireVisibleReplayText=*/true) ||
            TUInsertionCanDeferResyncToConditionalJoin(
                advancedOverSourceLineControlPrefix, tuPath, span.second,
                "TU");

        ResyncOutcome ro =
            skipLocalResync
                ? ResyncOutcome(padded, std::nullopt)
                : textEditAssembler_->ApplyResyncOrPend(
                      tuBytes, span.first, span.second, padded, tuPath);
        structuralHunkDispatcher.AddTUEdit(BuildDirectTUHunkTextEdit(
            h, i, span, std::move(ro), StringRef(padded), rawTUStart, rawTUEnd,
            materializedBByteBegin, materializedBByteEnd,
            AcceptedPathKind::TUByteSpanMappedEdit));
        continue;
      }
    }

    // f) Ownership resolution failed.
    //
    // Treat unresolved ownership as a theorem-boundary case rather than a hard
    // internal error: once macro ownership, include ownership, and truthful
    // TU ownership all fail, the engine may still salvage the edit via a
    // declared TU byte-span class. If that last deterministic TU realization
    // also fails, the code below requests the named terminal fallback
    // OwnerUnresolvedNoTUAnchor.
    //
    // Do not abort in strict mode here. Strict mode is enforced later by the
    // theorem audit and terminal-fallback machinery, not by crashing before
    // the explicit out-of-domain boundary is recorded.

    auto hunkConsumesNonTUMappedToken = [&]() -> bool {
      const auto &tokmapByPP = model_.GetTokmapByPP();
      for (uint64_t pp = h.aStart; pp < h.aEnd; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it == tokmapByPP.end())
          continue;
        if (!pathIdentity_.PathsEqual(it->second.file, tuPath))
          return true;
      }
      return false;
    };

    // A conservative TU byte-span edit may only be used when the whole consumed
    // hunk is TU-owned.  If the hunk also consumes tokens produced by a
    // top-level include, replacing only the TU subset would leave the original
    // include directive alive and replay stale tokens.  That hybrid case routes
    // through the declared TUIncludeClosureEdit proof; otherwise fail closed
    // instead of manufacturing an unsound partial TU edit.
    if (hunkConsumesNonTUMappedToken()) {
      llvm::SmallVector<std::pair<uint64_t, uint64_t>, 8>
          stagedSourceIntervals =
              structuralHunkDispatcher.BuildTUClosureSourceIntervals();
      if (auto closureEdit =
              expansionFallbackPlanner_->BuildTUIncludeClosureEditForUnresolvedHunk(
                  h, tuPath, tuBytes, stagedSourceIntervals)) {
        structuralHunkDispatcher.AddTUEdit(std::move(*closureEdit));
        continue;
      }

      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::OwnerClosedCover,
              TerminalFallbackFailureReason::NoOwnerClosedCover,
              TerminalFallbackFailureContext::ForHunkTokenEnvelope(
                  i, h.aStart, h.aEnd, h.bStart, h.bEnd)),
          "classify",
          ProofLattice().BuildOwnerUnresolvedNoTUAnchorDetail(i, h, tuPath, owner, mapsToTU));
      continue;
    }

    if (auto spanPlan =
            TUEditPlanner().PlanTUByteSpan(h.aStart, h.aEnd, tuPath)) { // [b, e)
      auto span = spanPlan->byteRange();
      std::string repl;
      const uint64_t rawTUStart = span.first;
      const uint64_t rawTUEnd = span.second;
      std::optional<uint64_t> materializedBByteBegin;
      std::optional<uint64_t> materializedBByteEnd;
      if (auto bBytes =
          sourceMapper_.BTokenRangeToByteRange(h.bStart, h.bEnd)) {
        materializedBByteBegin = bBytes->first;
        materializedBByteEnd = bBytes->second;
      }
      if (isDel) {
        repl = "";
      } else {
        StringRef bSlice =
            h.isInsertOnly()
                ? refoldSliceTokenEnvelope(bTokOff_, bSource_, h.bStart, h.bEnd)
                : refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                                h.bStart, h.bEnd);
        repl.assign(bSlice.data(), bSlice.data() + bSlice.size());
        if (h.isInsertOnly()) {
          // For pure insertions the source slice is the *token envelope*, not
          // just the exact token byte cover.  Zero-normal-token sideband lines
          // can live between the inserted ordinary tokens and the next normal
          // token.  Use the actual envelope bytes when partitioning replay so a
          // separately materialized sideband replacement/deletion is not also
          // emitted by this ordinary TU insertion.
          std::optional<uint64_t> envelopeBegin;
          std::optional<uint64_t> envelopeEnd;
          if (!bSlice.empty()) {
            envelopeBegin = static_cast<uint64_t>(bSlice.data() -
                                                  bSource_.data());
            envelopeEnd =
                *envelopeBegin + static_cast<uint64_t>(bSlice.size());
          }
          repl = textEditAssembler_->StripSeparatelyOwnedSidebandReplay(
              repl, envelopeBegin, envelopeEnd);
        }
      }

      // This patch inserts B text at a zero-width TU site: the TU span is
      // empty, but the hunk contributes one or more B tokens. Token-envelope
      // byte ranges begin at the first inserted token, so they do not include
      // any spaces or tabs that appear immediately before that token in B on
      // the same line. Preserve those preceding spaces/tabs when forming the
      // inserted text, unless equivalent spacing is already present immediately
      // to the left of the insertion point in the TU.
      if (!isDel && span.first == span.second && h.bStart < h.bEnd &&
          h.bStart > 0) {
        const size_t bTokStart = static_cast<size_t>(h.bStart);
        const size_t b0 = bTokOff_[bTokStart];
        size_t p = b0;
        while (p > 0) {
          char c = bSource_[p - 1];
          if (c == ' ' || c == '\t') {
            --p;
            continue;
          }
          break;
        }
        if (p < b0) {
          const bool tuHasSpaceLeft =
              span.first > 0 && (tuBytes[span.first - 1] == ' ' ||
                                  tuBytes[span.first - 1] == '\t');
          if (!tuHasSpaceLeft) {
            repl.insert(0, std::string(bSource_.data() + p, b0 - p));
            materializedBByteBegin = static_cast<uint64_t>(p);
          }
        }
      }

      bool consumedSeparatorGapForPunctuation = false;

      if (!isDel && h.isInsertOnly()) {
        consumedSeparatorGapForPunctuation =
            MaybeConsumeOrdinarySeparatorGapForPunctuation(
                tuPath, tuBytes, span, StringRef(repl), "TU conservative");
      }

      TUEditPlanner().MaybeExtendTUSpanOverClosedTrailingCallSuffix(
          h, tuPath, tuBytes, repl, span);

      bool advancedOverSourceLineControlPrefix =
          MaybeAdvanceTUInsertionPastSourceLineControlPrefix(
              h, tuPath, tuBytes, span, "TU conservative");

      // If we are replacing whitespace-only text in the TU, we prefer to
      // preserve the existing TU gap whitespace rather than introducing new
      // whitespace from B.
      bool replacingGap = false;
      if (span.first < span.second) {
        std::string original(tuBytes.data() + span.first,
                             tuBytes.data() + span.second);
        replacingGap = !original.empty() && stringutils::isWs(original);
        if (replacingGap && !consumedSeparatorGapForPunctuation) {
          // Preserve exactly the gap as the replacement.
          repl = std::move(original);
        }
      } else if (span.second < span.first) {
        REFOLD_LOG_FATAL("tu/span", "invalid TU byte span: [{0},{1})", span.first,
              span.second);
      }

      // If this patch replaces a non-empty whitespace gap in the TU, and the
      // replacement text does not already begin with whitespace, prefix a
      // single space so adjacent tokens remain separated. Add only one space,
      // even if the original gap was wider, to avoid duplicating spacing.
      if (replacingGap && !consumedSeparatorGapForPunctuation &&
          !repl.empty() && !stringutils::isWs(repl.front()))
        repl.insert(repl.begin(), ' ');

      // Keep a copy for logging; PadAtBoundaries consumes via move.
      std::string rawRepl = repl;

      std::string padded =
          PadAtBoundaries(tuBytes, static_cast<size_t>(span.first),
                          static_cast<size_t>(span.second), std::move(repl),
                          /*allowLeft*/ !replacingGap,
                          /*allowRight*/ true);


      const bool skipLocalResync =
          TUInsertionBeforeMaterializedInclude(
              h, tuPath, span, /*requireVisibleReplayText=*/false) ||
          TUInsertionCanDeferResyncToConditionalJoin(
              advancedOverSourceLineControlPrefix, tuPath, span.second,
              "TU conservative");

      ResyncOutcome ro =
          skipLocalResync
              ? ResyncOutcome(padded, std::nullopt)
              : textEditAssembler_->ApplyResyncOrPend(
                    tuBytes, span.first, span.second, padded, tuPath);
      structuralHunkDispatcher.AddTUEdit(BuildDirectTUHunkTextEdit(
          h, i, span, std::move(ro), StringRef(padded), rawTUStart, rawTUEnd,
          materializedBByteBegin, materializedBByteEnd,
          AcceptedPathKind::TUByteSpanConservativeEdit));
      continue;
    }

    // Before we escalate to the explicit terminal out-of-domain carrier, try
    // the declared TUIncludeClosureEdit class.  This source-closure proof keeps
    // already-proved structural work alive by materializing a closed run of
    // top-level TU `#include` directives directly into TU source text when that
    // run either exactly covers the unresolved PP hunk or can be widened to the
    // full include cover without absorbing another token diff.
    llvm::SmallVector<std::pair<uint64_t, uint64_t>, 8> stagedSourceIntervals =
        structuralHunkDispatcher.BuildTUClosureSourceIntervals();
    if (auto closureEdit =
            expansionFallbackPlanner_->BuildTUIncludeClosureEditForUnresolvedHunk(
                h, tuPath, tuBytes, stagedSourceIntervals)) {
      structuralHunkDispatcher.AddTUEdit(std::move(*closureEdit));
      continue;
    }

    // Use the explicit theorem-boundary interpretation for this last ownership
    // gap. By the time control reaches this branch, the engine has already
    // failed to prove a macro owner, include owner, truthful TU-owned byte
    // span, any exact/provable TU insertion anchor, and the declared
    // TUIncludeClosureEdit source-closure class. Do not manufacture a weaker
    // success class here; terminate via the named out-of-domain boundary
    // instead.
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::OwnerClosedCover,
            TerminalFallbackFailureReason::NoOwnerClosedCover,
            TerminalFallbackFailureContext::ForHunkTokenEnvelope(
                i, h.aStart, h.aEnd, h.bStart, h.bEnd)),
        "classify",
        ProofLattice().BuildOwnerUnresolvedNoTUAnchorDetail(i, h, tuPath, owner, mapsToTU));
    continue;
  }

  // Global fail-closed composition rule: once this single structural pass has
  // requested terminal fallback, do not continue composing structural
  // artifacts. The outer driver will discard the current attempt and emit B
  // directly.
  if (terminalSink_.HasRequest()) {
    REFOLD_LOG_DEBUG("fallback", "single-pass refold aborted after classification; "
                      "terminal fallback will be emitted");
    return std::string();
  }

  // Repair macro-state liveness after TU-level source edits.  The planner
  // owns the directive index, final TU edit interval queries, include ancestry
  // checks, proof witnesses, and conservative TU edit mutations used by all
  // macro-state repair phases.
  RefoldMacroStateRepairPlanner::MacroStateRepairRequest macroStateRepairRequest{
      tuPath, tuBytes, &structuralHunkDispatcher, &tuEdits};
  RefoldMacroStateRepairPlanner::MacroStateRepairPlan macroStateRepairPlan =
      MacroStateRepairPlanner().Plan(macroStateRepairRequest);
  if (!macroStateRepairPlan.Success)
    return std::string();

  // Inject forced __COUNTER__ patches after normal hunk attribution.
  // This may create macro patches even when no diff hunk touched the invocation
  // (required to prevent later __COUNTER__ values from shifting after an edit).
  auto forcedCounters =
      CounterStabilization().ComputeForcedCounterPatches(tuPath, abTokMapA2B_);
  auto forcedCountersFromExpanded =
      CounterStabilization().ComputeForcedCounterPatchesFromExpandedMacros(
          tuPath, structuralHunkDispatcher.MacroPatchMergeBucketsForPlanner());
  if (!forcedCountersFromExpanded.empty())
    forcedCounters.append(forcedCountersFromExpanded.begin(),
                          forcedCountersFromExpanded.end());
  if (!forcedCounters.empty())
    AddForcedCounterPatches(forcedCounters, structuralHunkDispatcher);

  auto sourceAuthoredLineControlDominatesSite =
      [&](const RefoldModel::MacroInvocation &site) -> bool {
    if (!site.invB || !site.invFile)
      return false;

    for (const RefoldModel::LineControlEvent &event :
         model_.GetLineControls()) {
      if (!event.active || !event.producerProven)
        continue;
      if (event.ownerIncludeId != site.ownerIncludeId)
        continue;
      if (!event.siteE || *event.siteE > *site.invB)
        continue;
      if (event.physicalFile.empty() || event.physicalFile.starts_with("<"))
        continue;
      if (!pathIdentity_.PathsEqual(event.physicalFile, *site.invFile))
        continue;

      // Only real source line controls discharge this source-preservation
      // case.  Built-in command-line line markers are recorded in the same
      // stream, but replaying them as source would not be a user-authored
      // repair dominating the surviving builtin spelling.
      StringRef text(event.text);
      text = text.ltrim();
      if (text.starts_with("#line") ||
          (text.size() >= 2 && text[0] == '#' &&
           stringutils::isNonNewlineWs(text[1])))
        return true;
    }

    return false;
  };

  auto replayContextSensitivePredefinedBuiltin =
      [&](StringRef name, const RefoldModel::MacroInvocation &site) {
    // These builtins produce replay-time or physical-file-timestamp literals
    // that cannot be repaired by synthetic #line state.
    if (name == "__TIMESTAMP__" || name == "__DATE__" ||
        name == "__TIME__")
      return true;

    if (name != "__BASE_FILE__")
      return false;

    // __BASE_FILE__ is normally part of the line/file observer model: a
    // preserved source-authored #line can make the spelling replay to the same
    // producer-observed value, and forcing B realization in those cases destroys
    // better source-preserving refoldings.  The remaining unsafe case is a
    // value-producing TU-local spelling that would otherwise require a synthetic
    // prologue solely to hide the checker output path.  Prefer the local
    // producer-proven literal for that single observer instead of adding a
    // global line directive at the top of the refolded file.
    if (site.ownerIncludeId)
      return false;
    if (!site.invFile || !pathIdentity_.PathsEqual(*site.invFile, tuPath))
      return false;
    if (sourceAuthoredLineControlDominatesSite(site))
      return false;
    return true;
  };

  auto forceReplayContextSensitivePredefinedBuiltinPatches = [&]() {
    size_t forcedPredefinedObserverPatches = 0;

    for (const RefoldModel::MacroInvocation &builtin :
         model_.GetMacroInvocations()) {
      // Only value-producing builtin expansions need literalization here.
      // Builtins used as source #line operands normally have no preprocessed
      // token cover of their own; they are consumed while computing line/file
      // state.  Forcing a whole-cover patch for such zero-token operands either
      // has no valid B envelope or unnecessarily destroys the source #line
      // structure that already repairs downstream observers.
      if (!builtin.cover.IsValid())
        continue;
      if (!lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(builtin))
        continue;

      const RefoldModel::MacroInvocation *site =
          lineControlProof_.LineStateObservableMacroSite(builtin);
      if (!site)
        continue;
      if (macroTopology_.IsInvocationInsideDefineDirective(*site))
        continue;
      if (!replayContextSensitivePredefinedBuiltin(builtin.name, *site))
        continue;

      // These predefined macros observe replay context that should not survive
      // as source spelling in the final output:
      //   * __TIMESTAMP__ names the timestamp of the physical file containing
      //     the spelling.
      //   * __DATE__ and __TIME__ name the time of the replay run itself.
      //   * A TU-local __BASE_FILE__ with no dominating source-authored #line
      //     would otherwise need a synthetic prologue to mask the checker output
      //     path.
      // Force a whole-cover macro realization so the emitted source carries the
      // producer-proven literal from B instead of depending on volatile replay
      // context or a gratuitous global line directive.
      const auto invStart = site->invB;
      const auto invEnd = site->invE;
      if (!invStart || !invEnd || *invEnd < *invStart)
        continue;

      std::optional<WholeCoverPlan> plan =
          MacroPatchPlanner().ComputeWholeCoverPlan(*site);
      if (!plan) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::ProducerFactsAvailable,
                TerminalFallbackFailureReason::MissingProducerFacts),
            "macro/predefined",
            llvm::formatv("replay-context-sensitive predefined macro '{0}' "
                          "invocation #{1} survived as source spelling, but "
                          "no whole-cover B realization plan is available",
                          builtin.name, site->id)
                .str());
        continue;
      }

      // Coalesce by physical invocation span, matching normal macro hunk
      // attribution.  Multiple volatile predefined builtins inside the same
      // enclosing macro callsite should force one deterministic whole-cover
      // realization for that callsite, not competing overlapping edits.
      RefoldStructuralHunkDispatcher::MacroPatchStagingSlot stagingSlot =
          structuralHunkDispatcher.PrepareMacroPatchStagingSlot(*site);

      MacroPatch patch{*invStart, *invEnd, plan->clippedText, site->id};
      if (stagingSlot.ExistingPatch)
        MacroPatchPlanner().CarryMacroPatchOwnerCertificate(
            patch, *stagingSlot.ExistingPatch);
      ProofLattice().StampMacroWholeCoverRealizationPatch(patch, *plan, *site);
      MacroPatchPlanner().StampMacroPatchOwnerWitness(
          patch, site->ownerIncludeId ? Owner::Include(*site->ownerIncludeId)
                                      : Owner::TU());
      structuralHunkDispatcher.StageMacroPatch(stagingSlot, std::move(patch));
      ++forcedPredefinedObserverPatches;
    }

    if (forcedPredefinedObserverPatches != 0) {
      REFOLD_LOG_TRACE(
          "macro/predefined",
          "forced {0} replay-context-sensitive predefined macro invocation(s) "
          "to B-surface realization",
          forcedPredefinedObserverPatches);
    }
  };

  forceReplayContextSensitivePredefinedBuiltinPatches();

  // Materialize merged macro patches into the list buckets expected by later
  // planning passes. The dispatcher owns the DenseMap merge buckets and flattens
  // them deterministically before final include/TU emission.
  structuralHunkDispatcher.FinalizeMacroPatchBuckets(ProofLattice());

  if (!structuralHunkDispatcher.AppendLineObserverRealizationEdits(
          LineObserverLayout(), tuPath, tuBytes))
    return std::string();


  // Normalize/coalesce include-side insertions.
  structuralHunkDispatcher.OrderIncludeInsertions();

  // 5) Schedule include materializations.  The scheduler owns the run-local
  // child index, materialization seed set, realized expansion cache, and
  // source-graph/line-control metadata shared by the early materialization
  // phase and the later TU-root include edit emission phase.
  RefoldIncludeMaterializationScheduler::Dependencies includeSchedulerDeps;
  includeSchedulerDeps.Model = &model_;
  includeSchedulerDeps.PathIdentity = &pathIdentity_;
  includeSchedulerDeps.IncludeMaterializer = includeMaterializer_.get();
  includeSchedulerDeps.IncludeInsertionPlanner = includeInsertionPlanner_.get();
  includeSchedulerDeps.LineObserverLayout = lineObserverLayout_.get();
  includeSchedulerDeps.MacroStateRepairPlanner = macroStateRepairPlanner_.get();
  includeSchedulerDeps.TextEditAssembler = textEditAssembler_.get();
  includeSchedulerDeps.ProofLattice = proofLattice_.get();
  includeSchedulerDeps.TerminalSink = &terminalSink_;
  includeSchedulerDeps.SidebandPragmaEdits = &sidebandPragmaEdits_;

  RefoldIncludeMaterializationScheduler::IncludeMaterializationRequest
      includeSchedulerRequest;
  includeSchedulerRequest.TUPath = tuPath;
  includeSchedulerRequest.TUBytes = tuBytes;
  includeSchedulerRequest.ASource = aSource_;
  includeSchedulerRequest.BSource = bSource_;
  includeSchedulerRequest.ATokens = aToks_;
  includeSchedulerRequest.BTokens = bToks_;
  includeSchedulerRequest.ATokenOffsets = aTokOff_;
  includeSchedulerRequest.BTokenOffsets = bTokOff_;
  includeSchedulerRequest.TokenHunks = hunks;
  includeSchedulerRequest.RawByteHunks = abByteHunks_ ? &*abByteHunks_ : nullptr;
  includeSchedulerRequest.StructuralHunkDispatcher = &structuralHunkDispatcher;
  includeSchedulerRequest.SourceGraphOutputs = sourceGraphOutputs_;

  RefoldIncludeMaterializationScheduler includeMaterializationScheduler(
      std::move(includeSchedulerDeps), std::move(includeSchedulerRequest));
  if (!includeMaterializationScheduler.MaterializeIncludeExpansions())
    return std::string();

  lastStats_.expandedIncludes =
      includeMaterializationScheduler.ExpandedIncludeIds().size();

  // 6) Final TU emission.  The emission planner owns TU macro edit
  // lowering, the second macro-state carry pass, TU-root include edit staging,
  // pending-resync-aware text assembly, final line-control prologue repair, and
  // final expanded-macro statistics.
  RefoldFinalTUEmissionPlanner::Dependencies finalEmissionDeps;
  finalEmissionDeps.Model = &model_;
  finalEmissionDeps.MacroTopology = &macroTopology_;
  finalEmissionDeps.LineControlProof = &lineControlProof_;
  finalEmissionDeps.LineDirs = &lineDirs_;
  finalEmissionDeps.MacroStateRepairPlanner = macroStateRepairPlanner_.get();
  finalEmissionDeps.TextEditAssembler = textEditAssembler_.get();
  finalEmissionDeps.ProofLattice = proofLattice_.get();
  finalEmissionDeps.TerminalSink = &terminalSink_;
  finalEmissionDeps.MaterializedEditMappings = materializedEditMappings_;
  finalEmissionDeps.FinalLineControlPruneCandidates =
      &finalLineControlPruneCandidates_;
  finalEmissionDeps.FinalLineControlSourceMappings =
      &finalLineControlSourceMappings_;

  RefoldFinalTUEmissionPlanner::EmissionRequest finalEmissionRequest;
  finalEmissionRequest.TUPath = tuPath;
  finalEmissionRequest.TUBytes = tuBytes;
  finalEmissionRequest.StructuralHunkDispatcher = &structuralHunkDispatcher;
  finalEmissionRequest.IncludeMaterializationScheduler =
      &includeMaterializationScheduler;
  finalEmissionRequest.MacroStatePlan = &macroStateRepairPlan;
  finalEmissionRequest.MacroStateRequest = &macroStateRepairRequest;

  RefoldFinalTUEmissionPlanner finalEmissionPlanner(
      std::move(finalEmissionDeps));
  RefoldFinalTUEmissionPlanner::EmissionResult finalEmission =
      finalEmissionPlanner.PlanAndEmit(finalEmissionRequest);
  if (!finalEmission.Success)
    return std::string();

  lastStats_.expandedMacros = finalEmission.ExpandedMacroCount;
  return std::move(finalEmission.TUText);
}

// =========================== Edit ordering helpers =========================

std::optional<uint64_t> RefoldEngine::ByteStartForPPInFile(StringRef file,
                                                           uint64_t pp) const {
  auto it = model_.GetTokmapByPP().find(pp);
  if (it != model_.GetTokmapByPP().end() &&
      pathIdentity_.PathsEqual(it->second.file, file))
    return it->second.b;
  return std::nullopt;
}

std::optional<uint64_t> RefoldEngine::ByteEndForPPInFile(StringRef file,
                                                         uint64_t pp) const {
  auto it = model_.GetTokmapByPP().find(pp);
  if (it != model_.GetTokmapByPP().end() &&
      pathIdentity_.PathsEqual(it->second.file, file))
    return it->second.e;
  return std::nullopt;
}


// ============================= Boundary helpers ==============================

namespace {
/// Return true iff \p kind is separator punctuation that may legitimately
/// replace a horizontal source gap between two tokens.
///
/// This is intentionally narrower than "left-attachable punctuation": closing
/// delimiters and operators can carry context-sensitive spacing conventions, so
/// they are not treated as gap replacements here. Callers must still prove with
/// `refoldNeedsLexicalSeparator()` that attaching the punctuation to the
/// token on its left preserves lexical tokenization.
static bool isSeparatorGapReplacementPunctuation(tok::TokenKind kind) {
  switch (kind) {
  case tok::comma:
  case tok::semi:
  case tok::colon:
    return true;
  default:
    return false;
  }
}
} // namespace

/// Compatibility wrapper for the shared lexical boundary-padding helper.
/// New code should call refoldPadAtBoundaries directly when it already has the
/// relevant owner-local byte stream and language options.
std::string RefoldEngine::PadAtBoundaries(StringRef base, size_t start,
                                          size_t end, std::string text,
                                          bool allowLeft,
                                          bool allowRight) const {
  return refoldPadAtBoundaries(base, start, end, std::move(text), allowLeft,
                               allowRight, lexLang_);
}


// ===================== Patch builders (include & macro) ======================

void RefoldEngine::AddForcedCounterPatches(
    ArrayRef<ForcedMacroPatchRequest> forced,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) const {
  struct CounterReplacementSurface {
    std::string text;
    uint64_t bTokStart = 0;
    uint64_t bTokEnd = 0;
  };

  auto buildOccReplacement =
      [&](uint64_t aStart,
          uint64_t aEnd) -> std::optional<CounterReplacementSurface> {
    // For __COUNTER__, the replacement is tied to the specific expanded
    // occurrence, not to reusable macro-body text. Map that occurrence's
    // A-token range into B and use the resulting B spelling directly.  Keep
    // the B-token envelope together with the text so the accepted witness can
    // prove target-PP identity without relying on the spelling preview.
    auto bEnv = sourceMapper_.MapATokRangeAToBTokenEnvelope(aStart, aEnd);
    if (!bEnv)
      return std::nullopt;
    if (bEnv->second <= bEnv->first)
      return std::nullopt;

    CounterReplacementSurface surface;
    surface.text = sourceMapper_.SliceBSource(bEnv->first, bEnv->second).trim().str();
    surface.bTokStart = static_cast<uint64_t>(bEnv->first);
    surface.bTokEnd = static_cast<uint64_t>(bEnv->second);
    return surface;
  };

  auto deriveWholeCoverBTokenRange = [&](const RefoldModel::MacroInvocation &m)
      -> std::optional<std::pair<uint64_t, uint64_t>> {
    std::optional<std::pair<uint64_t, uint64_t>> cover =
        MacroPatchPlanner().GetWholeCoverATokRange(m);
    if (!cover)
      return std::nullopt;

    std::optional<std::pair<size_t, size_t>> bEnv =
        sourceMapper_.MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(cover->first,
                                                                cover->second);
    if (!bEnv || bEnv->second <= bEnv->first || bEnv->second > bToks_.size())
      return std::nullopt;

    return std::make_pair(static_cast<uint64_t>(bEnv->first),
                          static_cast<uint64_t>(bEnv->second));
  };

  for (const auto &req : forced) {
    const RefoldModel::MacroInvocation *pm = req.macro;
    if (!pm)
      continue;
    const RefoldModel::MacroInvocation &m = *pm;

    // Safety: do not patch macro definitions.
    if (macroTopology_.IsInvocationInsideDefineDirective(m))
      continue;

    // Forced patches still require a concrete physical invocation span; without
    // it there is no call-site text to replace.
    const auto invStart = m.invB;
    const auto invEnd = m.invE;
    if (!invStart || !invEnd || *invEnd < *invStart)
      continue;

    std::optional<CounterReplacementSurface> counterSurface;
    std::optional<std::string> nonCounterReplacement;
    if (m.name == "__COUNTER__") {
      counterSurface = buildOccReplacement(req.aStart, req.aEnd);
    } else {
      // Other forced counter-related requests use the normal whole-cover text
      // builder so they remain aligned with whole macro invocation replay.
      nonCounterReplacement = ProofLattice().BuildWholeCoverReplacementText(m);
    }
    if (!counterSurface && !nonCounterReplacement)
      continue;

    // Coalesce by physical invocation span (inv_b/inv_e), matching the hunk
    // coalescing logic used during normal classification.
    RefoldStructuralHunkDispatcher::MacroPatchStagingSlot stagingSlot =
        structuralHunkDispatcher.PrepareMacroPatchStagingSlot(m);

    // Preserve an existing non-callsite (already-expanded) replacement.
    if (stagingSlot.ExistingPatch && !stagingSlot.ExistingIsCallsite)
      continue;

    CounterEventIdentity counterEventForWitness;
    SuffixStabilityWitness counterWitness = SuffixStabilityWitness::None();
    {
      CounterEventIdentity event = req.event;
      if (event.macroInvocationId == 0)
        event = macroTopology_.BuildCounterEventIdentity(
            m, /*occurrenceOrdinal=*/0, req.aStart, req.aEnd, m.ownerIncludeId);
      if (!event.expectedBValue) {
        if (counterSurface)
          event.expectedBValue = counterSurface->text;
        else if (nonCounterReplacement)
          event.expectedBValue = *nonCounterReplacement;
      }
      counterEventForWitness = event;
      const OwnerStateBoundary boundary = OwnerStateProof().CounterStateBoundaryForEvent(event);
      const std::string detail =
          llvm::formatv(
              "forced materialization of counter-sensitive invocation "
              "#{0} after edited counter occurrence A=[{1},{2})",
              m.id, req.aStart, req.aEnd)
              .str();
      counterWitness = OwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::OwnerMaterialization,
          OwnerStateComponent::Counter, boundary,
          llvm::formatv("{0}; {1}", detail, OwnerStateProof().FormatCounterEventForWitness(event))
              .str());
      (void)OwnerStateProof().CheckStateTransitionAcrossEditBoundary(
          boundary, OwnerStateComponent::Counter,
          StateMutationKind::Materialized, counterWitness, "counter", detail,
          /*requireKnownObserver=*/false);
    }

    // Install the forced patch under the coalesced physical-span key. If this
    // replaces a previous call-site-shaped patch, carry its owner certificate
    // forward before stamping the current invocation owner.
    std::optional<std::pair<uint64_t, uint64_t>> materializedBTokenRange;
    if (counterSurface) {
      materializedBTokenRange =
          std::make_pair(counterSurface->bTokStart, counterSurface->bTokEnd);
    } else if (nonCounterReplacement) {
      // Enclosing macro materializations are whole-cover counter-stabilization
      // repairs. Their target-PP proof is the exact B-token envelope of the
      // whole macro expansion, not the replacement string constructed from it.
      materializedBTokenRange = deriveWholeCoverBTokenRange(m);
    }

    std::string replacementText = counterSurface
                                      ? std::move(counterSurface->text)
                                      : std::move(*nonCounterReplacement);
    MacroPatch patch{*invStart, *invEnd, std::move(replacementText)};
    if (materializedBTokenRange &&
        materializedBTokenRange->first <= materializedBTokenRange->second &&
        materializedBTokenRange->second <= bToks_.size()) {
      patch.hasMaterializedBTokenRange = true;
      patch.materializedBTokStart = materializedBTokenRange->first;
      patch.materializedBTokEnd = materializedBTokenRange->second;
    }
    if (stagingSlot.ExistingPatch)
      MacroPatchPlanner().CarryMacroPatchOwnerCertificate(
          patch, *stagingSlot.ExistingPatch);
    MacroPatchPlanner().StampMacroPatchOwnerWitness(
        patch, m.ownerIncludeId ? Owner::Include(*m.ownerIncludeId)
                                : Owner::TU());

    // Forced __COUNTER__ stabilization is an invocation-realization proof, not
    // an anonymous text edit.  The forced root is often the spelling of
    // __COUNTER__ itself, but it can also be an enclosing macro invocation
    // whose expansion observes __COUNTER__ (for example PRINT(...) wrapping
    // __COUNTER__).  In both cases the emitted patch is required solely to keep
    // the counter sequence consistent after an earlier counter occurrence was
    // realized, so stamp every forced counter-stabilization patch with the
    // explicit counter proof class and its typed state-stability witness.
    MacroPatchProof proof =
        ProofLattice().MakeMacroPatchProof(MacroPatchProofKind::CounterLiteral,
                            /*preservesInvocationStructure=*/false, m.id);
    CounterStateWitness counterState;
    counterState.hasCounterEvents = true;
    counterState.counterOrderKnown = true;
    counterState.suffixStateStable = true;
    counterState.materializationStable = true;
    counterState.counterConsumptionCount = 1;
    counterState.counterMutationCount = 1;
    if (counterWitness.kind != SuffixStabilityWitnessKind::None)
      counterState.preservedSuffixObserverCount = 1;
    if (counterEventForWitness.expectedBValue) {
      counterState.hasExpectedBValues = true;
      counterState.expectedBValueCount = 1;
      counterState.suffixValueSignature =
          llvm::formatv(
              "expected={0}",
              RefoldProofLattice::FormatWitnessTraceHash(*counterEventForWitness.expectedBValue))
              .str();
    } else {
      counterState.hasMissingExpectedBValues = true;
      counterState.missingExpectedBValueCount = 1;
    }
    counterState.consumptionSignature =
        OwnerStateProof().FormatCounterEventForWitness(counterEventForWitness);
    counterState.orderSignature =
        llvm::formatv("ordinal={0}:macro={1}:A=[{2},{3})",
                      counterEventForWitness.occurrenceOrdinal,
                      counterEventForWitness.macroInvocationId,
                      counterEventForWitness.aTokenBegin,
                      counterEventForWitness.aTokenEnd)
            .str();
    counterState.suffixObserverSignature =
        llvm::formatv("forced-materialization:{0}", counterWitness.kind).str();
    proof.suffixStability = std::move(counterWitness);
    proof.counterState = std::move(counterState);
    ProofLattice().SetMacroPatchProof(patch, std::move(proof));

    // Use the coalesced key as the patch macro ID so later owner/macro maps see
    // one canonical patch per physical invocation span.
    structuralHunkDispatcher.StageMacroPatch(stagingSlot, std::move(patch));
  }
}

// Whole-cover realization is admitted only by MacroWholeCoverIsSelfContained(),
// which proves the selected A-token cover directly from the invocation's own
// detailed provenance spans before stamping the shared OwnerRealizationProof. A
// nested child whose cover needs caller/descendant aggregation must instead be
// handled by structure-preserving DAG lifting or by a wider owner realization;
// coarse descendant span aggregation is not a separate proof class.
// Final line-control audit helpers validate engine-owned final-pruning state
// from the top-level refold orchestration.  Refold() calls them directly to
// enforce the no-legacy theorem audit contract for the final line-control pass.
bool RefoldEngine::AuditFinalLineControlAuthorityContract(
    const FinalLineControlAuthorityContract &authority, StringRef role) const {
  if (!TheoremAudit().IsNoLegacyAuditEnabled())
    return true;

  auto report = [&](StringRef detail) {
    TheoremAudit().ReportNoLegacyAuditFinding(
        RefoldTheoremAudit::MakeLegacyAuditEvidence(
            LegacyPathKind::FinalLineControlLivenessWithoutObligation, role,
            detail));
  };

  if (!authority.compactRemovalProofIsAuthoritative)
    report("final line-control compact removal proof is not authoritative");
  if (!authority.fixedPointPruningIsAuthoritative)
    report("final line-control fixed-point pruning is not authoritative");
  if (!authority.validationCallbackIsAuthoritative)
    report("final line-control validation callback is not authoritative");

  return authority.IsClosedUnderCompactProofs();
}

bool RefoldEngine::AuditFinalLineControlRemovalProofPopulation(
    ArrayRef<FinalLineControlPruneCandidate> candidates, StringRef role) const {
  if (!TheoremAudit().IsNoLegacyAuditEnabled())
    return true;

  size_t missing = 0;
  for (const FinalLineControlPruneCandidate &candidate : candidates)
    if (!HasCompleteFinalLineControlProof(candidate))
      ++missing;

  if (missing == 0)
    return true;

  TheoremAudit().ReportNoLegacyAuditFinding(
      RefoldTheoremAudit::MakeLegacyAuditEvidence(
          LegacyPathKind::FinalLineControlLivenessWithoutObligation, role,
          llvm::formatv(
              "{0} final line-control prune candidate(s) lack compact "
              "obligation/removal proof; generation sites must populate both facts "
              "before compact final-line-control pruning may run",
              missing)
              .str()));
  return false;
}

bool RefoldEngine::MaybeAdvanceTUInsertionPastSourceLineControlPrefix(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    std::pair<uint64_t, uint64_t> &span, StringRef tracePrefix) const {
  if (!h.isInsertOnly() || span.first != span.second ||
      TUEditPlanner().IsPPGapAtSelectedConditionalArmExit(h.aStart))
    return false;

  std::optional<uint64_t> exactAnchor =
      TUEditPlanner().AnchorToExactSlotBoundaryFromPPGap(tuPath, h.aStart);
  if (!exactAnchor || *exactAnchor != span.first)
    return false;

  std::optional<uint64_t> advancedAnchor =
      lineControlProof_.AdvanceInsertionAnchorPastSourceLineControlPrefix(tuPath, std::nullopt,
                                                        tuBytes, span.first);
  if (!advancedAnchor)
    return false;

  span.first = *advancedAnchor;
  span.second = *advancedAnchor;
  return true;
}

bool RefoldEngine::TUInsertionCanDeferResyncToConditionalJoin(
    bool advancedOverSourceLineControlPrefix, StringRef tuPath, uint64_t anchor,
    StringRef tracePrefix) const {
  if (!advancedOverSourceLineControlPrefix)
    return false;

  std::optional<LineStateObserverSite> firstObserver =
      lineControlProof_.FirstOwnerSuffixLineStateObserverSite(std::nullopt, tuPath, anchor);
  if (!firstObserver || !firstObserver->demand.needsLine)
    return false;

  const RefoldModel::CondGroup *innermost = nullptr;
  for (const RefoldModel::CondGroup *group :
       model_.GetCondGroups(tuPath, std::nullopt)) {
    if (!group || !pathIdentity_.PathsEqual(group->file, tuPath) || group->parentIncludeId ||
        !group->ContainsByte(anchor)) {
      continue;
    }
    if (!innermost || (group->groupB >= innermost->groupB &&
                       group->groupE <= innermost->groupE)) {
      innermost = group;
    }
  }

  if (!innermost || firstObserver->offset < innermost->groupE)
    return false;

  return true;
}


} // namespace refold
} // namespace clang

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
// focused proof/witness carrier headers.  An emitted edit is either an
// in-domain accepted result with a declared AcceptedProofClass, or an explicit
// terminal out-of-domain result with a named TerminalFallbackProofFailure.  No
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

#include "core/RefoldEngine.h"

#include "core/RefoldLangOptions.h"
#include "core/RefoldLog.h"
#include "core/RefoldOwnerClassifier.h"
#include "edit/RefoldBInsertionLedger.h"
#include "edit/RefoldExpansionFallbackPlanner.h"
#include "edit/RefoldFinalTUEmissionPlanner.h"
#include "edit/RefoldTUAnchorProof.h"
#include "edit/RefoldTUEditPlanner.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/IncludeSpellingHelpers.h"
#include "include/RefoldIncludeInsertionPlanner.h"
#include "include/RefoldIncludeMaterializationScheduler.h"
#include "include/RefoldIncludeMaterializer.h"
#include "include/RefoldPragmaOnceGuardRewriter.h"
#include "include/RefoldIncludeReplayProof.h"
#include "include/RefoldSourceGraphProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/RefoldLineControlProof.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldCounterStabilization.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroStateRepairPlanner.h"
#include "proof/RefoldNeutralityProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "sideband/RefoldSidebandPragmaEdits.h"
#include "source/RefoldMixedOwnerTilingPlanner.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldPreprocessingStructureIndexProvider.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "source/RefoldTokenDiffPlanner.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
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
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Verify that every durable structural segment binding names one exact
/// normalized hunk and the matching token edge in its parent witness.
///
/// The tiler lowers proof-rich structural partitions back into ordinary
/// `diffutils::Hunk` values.  Insertion provenance and owner dispatch index
/// those lowered hunks by position, so accepting a stale binding from an
/// intermediate normalization would silently attach the wrong proof carrier.
/// This check is deliberately independent of owner classification: it proves
/// only that the normalization transaction published one coherent hunk/ledger
/// snapshot before downstream planning begins.
bool structuralTilingLedgersMatchNormalizedHunks(
    ArrayRef<diffutils::Hunk> hunks,
    ArrayRef<MixedOwnerTilingWitness> witnesses,
    ArrayRef<MixedOwnerTilingSegmentBinding> bindings, std::string &failure) {
  for (const MixedOwnerTilingSegmentBinding &binding : bindings) {
    if (binding.witnessIndex >= witnesses.size()) {
      failure = llvm::formatv(
                    "binding witness index {0} is outside ledger size {1}",
                    binding.witnessIndex, witnesses.size())
                    .str();
      return false;
    }

    const MixedOwnerTilingWitness &witness =
        witnesses[binding.witnessIndex];
    if (witness.witnessId != binding.parentTilingWitnessId) {
      failure = llvm::formatv(
                    "binding witness id {0} does not match ledger id {1}",
                    binding.parentTilingWitnessId, witness.witnessId)
                    .str();
      return false;
    }
    if (binding.segmentIndex >= witness.edges.size()) {
      failure = llvm::formatv(
                    "binding segment index {0} is outside witness edge count "
                    "{1}",
                    binding.segmentIndex, witness.edges.size())
                    .str();
      return false;
    }

    const MixedOwnerTilingSegmentWitness &edge =
        witness.edges[binding.segmentIndex];
    if (edge.kind != MixedOwnerTilingEdgeKind::TokenSegment ||
        edge.aStart != binding.aStart || edge.aEnd != binding.aEnd ||
        edge.bStart != binding.bStart || edge.bEnd != binding.bEnd) {
      failure = llvm::formatv(
                    "binding A=[{0},{1}) B=[{2},{3}) does not match token "
                    "edge #{4} of witness {5}",
                    binding.aStart, binding.aEnd, binding.bStart, binding.bEnd,
                    binding.segmentIndex, binding.parentTilingWitnessId)
                    .str();
      return false;
    }

    size_t matchingHunkCount = 0;
    for (const diffutils::Hunk &hunk : hunks) {
      if (hunk.aStart == binding.aStart && hunk.aEnd == binding.aEnd &&
          hunk.bStart == binding.bStart && hunk.bEnd == binding.bEnd) {
        ++matchingHunkCount;
      }
    }
    if (matchingHunkCount != 1) {
      failure = llvm::formatv(
                    "binding A=[{0},{1}) B=[{2},{3}) matches {4} normalized "
                    "hunks instead of exactly one",
                    binding.aStart, binding.aEnd, binding.bStart, binding.bEnd,
                    matchingHunkCount)
                    .str();
      return false;
    }
  }

  // Every emitted token edge must also have exactly one reverse binding.  The
  // forward check above rejects stale bindings; this reverse check rejects a
  // partially published witness whose segment would reach owner dispatch
  // without the structural authority that justified its split.
  for (size_t witnessIndex = 0; witnessIndex < witnesses.size();
       ++witnessIndex) {
    const MixedOwnerTilingWitness &witness = witnesses[witnessIndex];
    for (size_t edgeIndex = 0; edgeIndex < witness.edges.size(); ++edgeIndex) {
      const MixedOwnerTilingSegmentWitness &edge = witness.edges[edgeIndex];
      if (edge.kind != MixedOwnerTilingEdgeKind::TokenSegment)
        continue;

      size_t matchingBindingCount = 0;
      for (const MixedOwnerTilingSegmentBinding &binding : bindings) {
        if (binding.witnessIndex == witnessIndex &&
            binding.parentTilingWitnessId == witness.witnessId &&
            binding.segmentIndex == edgeIndex) {
          ++matchingBindingCount;
        }
      }
      if (matchingBindingCount != 1) {
        failure = llvm::formatv(
                      "token edge #{0} of witness {1} has {2} reverse "
                      "bindings instead of exactly one",
                      edgeIndex, witness.witnessId, matchingBindingCount)
                      .str();
        return false;
      }
    }
  }

  return true;
}

} // namespace

RefoldEngine::RefoldEngine(
    RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool noLines, bool strict,
    ProofAuditMode proofAuditMode, StringRef finalOutputPath,
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    std::vector<MaterializedEditMapping> *materializedEditMappings,
    FinalLineControlValidationCallback finalLineControlValidationCallback,
    std::vector<SourceGraphOutput> *sourceGraphOutputs,
    std::optional<AlignmentSelectionOverride> alignmentSelectionOverride,
    bool alignmentSemanticResolverEnabled,
    std::optional<StringRef> tuSourceBytesOverride)
    : model_(std::move(model)), aSource_(aSource), bSource_(bSource),
      aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff),
      noLines_(noLines), finalOutputPath_(finalOutputPath.str()),
      lineDirs_(!noLines, model_.GetPPCwd()),
      pathIdentity_(model_, model_.GetPPCwd(), /*emitAbsPaths=*/false),
      strict_(strict), proofAuditMode_(proofAuditMode),
      lexLang_(makeRefoldLexLangOptions(model_.GetPPLang())),
      argTextRecovery_(lexLang_), tokenTextAnalysis_(lexLang_),
      terminalSink_(RefoldTerminalProofSinkCallbacks{
          [this](const TerminalFallbackProofFailure &failure, StringRef role) {
            TheoremAudit().AuditTerminalFallbackForLegacyAuthority(failure,
                                                                   role);
          },
          [this](StringRef detail) {
            TheoremAudit().NoteTheoremAuditViolation(detail);
          },
          [this](const TerminalFallbackRequest &request) {
            ProofLattice().WitnessTrace().TraceWitnessFallback(request);
          }}),
      finalReplaySurface_(buildFinalReplaySurface(model_, finalOutputPath)),
      materializedEditMappings_(materializedEditMappings),
      sourceGraphOutputs_(sourceGraphOutputs),
      finalLineControlValidationCallback_(
          std::move(finalLineControlValidationCallback)),
      sidebandPragmaEdits_(sidebandPragmaEdits.begin(),
                           sidebandPragmaEdits.end()),
      sourceMapper_(model_, pathIdentity_, aSource_, bSource_, aToks_, bToks_,
                    aTokOff_, bTokOff_, abTokHunks_, abByteHunks_,
                    abByteHunkPrefixDelta_, strict_),
      macroTopology_(model_, aToks_, bToks_, sourceMapper_, pathIdentity_),
      macroBoundarySelector_(model_, macroTopology_, sourceMapper_, bToks_,
                             lexLang_),
      lineControlProof_(model_, pathIdentity_, macroTopology_, lineDirs_,
                        aToks_, bToks_, abTokMapA2B_, abTokMapB2A_) {
  alignmentSelectionOverride_ = std::move(alignmentSelectionOverride);
  alignmentSemanticResolverEnabled_ = alignmentSemanticResolverEnabled;
  alignmentSemanticTheoremActive_ = alignmentSelectionOverride_.has_value();
  tuSourceBytesOverride_ = std::move(tuSourceBytesOverride);

  // Build the object graph in dependency order.  Each service receives
  // explicit inputs/services and, where orchestration remains engine-owned, a
  // narrow hook bundle; no service stores RefoldEngine itself.
  InitializeTheoremAudit();
  InitializeOwnerStateProof();
  InitializeMacroStateProof();
  InitializePreprocessingStructureIndex();
  InitializePreprocessingStructureIndexProvider();
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
  InitializePragmaOnceGuardRewriter();
  InitializeIncludeMaterializer();
  InitializeExpansionFallbackPlanner();
}

RefoldEngine::~RefoldEngine() = default;

// ========================== Public entry points ==========================

Expected<std::string> RefoldEngine::Refold(
    const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool noLines, bool strict,
    ProofAuditMode proofAuditMode, StringRef finalOutputPath,
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    std::vector<MaterializedEditMapping> *materializedEditMappings,
    FinalLineControlValidationCallback finalLineControlValidationCallback,
    std::vector<SourceGraphOutput> *sourceGraphOutputs) {
  // Build the refold model based on the parsed JSON object.
  auto mOrErr = RefoldModel::FromJson(rootJson);
  if (!mOrErr)
    return mOrErr.takeError();

  // Construct an engine and run the instance pipeline.
  RefoldEngine engine(
      std::move(*mOrErr), aSource, aToks, aTokOff, bSource, bToks, bTokOff,
      noLines, strict, proofAuditMode, finalOutputPath, sidebandPragmaEdits,
      materializedEditMappings, std::move(finalLineControlValidationCallback),
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

  // The engine first attempts structural refolding, then resolves the
  // post-structural fallback choice between proved intermediate expansion and
  // the explicit raw-B terminal carrier when proof discharge requests fallback.
  terminalSink_.Reset();
  resetRefoldAttemptStats(lastStats_, model_);
  TheoremAudit().Reset();

  std::string out = RunRefoldPass();

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

  // Producer line-control and builtin-observer facts are not lowered into a
  // passive final-stream model here; generation sites have already attached
  // the compact obligation/removal proof records consumed by the pruner below.

  AuditFinalLineControlRemovalProofPopulation(
      TheoremAudit(), finalLineControlPruneCandidates_,
      "final-line-control-prune-candidates");

  FinalLineControlPruneResult finalLinePrune =
      PruneFinalLineControlDirectives(out, finalLineControlPruneCandidates_,
                                      finalLineControlValidationCallback_);
  AuditFinalLineControlAuthorityContract(
      TheoremAudit(), finalLinePrune.authority, "final-line-control-prune");

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

  emitRefoldAttemptStatsSummary(lastStats_, terminalSink_.HasRequest());
  emitTheoremAuditSummary(lastTheoremAudit_);
  return out;
}

bool RefoldEngine::ValidateTokenCount() {
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
    return false;
  }
  return true;
}

std::unique_ptr<llvm::MemoryBuffer>
RefoldEngine::LoadTUSource(StringRef tuPath) {
  if (tuSourceLoadError_)
    REFOLD_LOG_FATAL("src/load", "{0}", *tuSourceLoadError_);

  // Reuse the exact byte snapshot indexed during service-graph construction.
  // Re-reading the file here would permit a filesystem race in which
  // PlanTUByteSpan() proves ranges against one version of the TU while final
  // edit assembly applies them to another.  A private MemoryBuffer copy keeps
  // the existing caller lifetime contract while preserving byte identity with
  // the preprocessing-structure census.
  return MemoryBuffer::getMemBufferCopy(tuSourceBytes_, tuPath);
}

std::vector<diffutils::Hunk>
RefoldEngine::PlanTokenDiff(StringRef tuPath) {
  // PlanTokenDiff is the only phase boundary allowed to expose structural
  // hunks.  Re-entry would permit an insertion ledger or owner classifier to
  // retain indices into a stale normalization, so reject it as an internal
  // pipeline violation rather than trying to rebuild partially consumed state.
  if (structuralHunkPlanningPhase_ !=
      StructuralHunkPlanningPhase::NotStarted) {
    REFOLD_LOG_FATAL(
        "plan/order",
        "token-diff planning entered from invalid structural phase {0}",
        static_cast<unsigned>(structuralHunkPlanningPhase_));
  }

  // 1) Build the initial A/B token diff and refresh the per-run diff caches.
  //
  // RefoldTokenDiffPlanner owns lexeme mapping, LCS provenance construction,
  // normalized token-hunk derivation, A/B token map population, and raw
  // byte-hunk cache construction.  Owner-aware tiling starts from this
  // deterministic token-diff plan below.
  assert(tokenDiffPlanner_ && "token diff planner service not initialized");
  RefoldTokenDiffPlanner::TokenDiffPlan diffPlan = tokenDiffPlanner_->Plan();

  // Production non-forced anchors must resolve to one durable semantic
  // witness retained by this engine run. Isolated candidate simulations carry
  // a temporary theorem-bearing override and are validated by the outer
  // resolver instead.
  if (!alignmentSelectionOverride_) {
    std::set<uint64_t> semanticWitnessIds;
    for (const AlignmentSemanticResolutionWitness &witness :
         alignmentSemanticResolutionWitnesses_) {
      if (witness.witnessId == 0 ||
          !semanticWitnessIds.insert(witness.witnessId).second) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "alignment semantic witness ledger has an invalid duplicate id");
      }
      if (!witness.completeEnumeration || witness.enumeratedMapCount == 0 ||
          witness.acceptedMapCount == 0 ||
          witness.acceptedMapCount + witness.rejectedMapCount !=
              witness.enumeratedMapCount ||
          witness.equivalenceKey.empty() ||
          witness.representativeMap.size() != abTokMapA2B_.size()) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "alignment semantic witness {0} is incomplete or malformed",
            witness.witnessId);
      }

      std::set<std::pair<uint64_t, uint64_t>> anchorEvidence;
      for (const AlignmentSemanticAnchorEvidence &evidence :
           witness.anchorEvidence) {
        if (evidence.aToken >= witness.representativeMap.size() ||
            witness.representativeMap[evidence.aToken] !=
                static_cast<int64_t>(evidence.bToken) ||
            !anchorEvidence.insert({evidence.aToken, evidence.bToken}).second) {
          REFOLD_LOG_FATAL(
              "lcs/semantic-resolver",
              "alignment semantic witness {0} has malformed anchor evidence",
              witness.witnessId);
        }
      }
      for (size_t aToken = 0; aToken < witness.representativeMap.size();
           ++aToken) {
        const int64_t bToken = witness.representativeMap[aToken];
        if (bToken < 0 ||
            (aToken < abTokAnchorProofs_.size() &&
             abTokAnchorProofs_[aToken].kind ==
                 diffutils::LcsAnchorProofKind::CoreOptimalPathForced))
          continue;
        if (!anchorEvidence.count(
                {aToken, static_cast<uint64_t>(bToken)})) {
          REFOLD_LOG_FATAL(
              "lcs/semantic-resolver",
              "alignment semantic witness {0} lacks evidence for A-token {1}",
              witness.witnessId, aToken);
        }
      }
    }
    for (size_t aToken = 0; aToken < abTokAnchorProofs_.size(); ++aToken) {
      const diffutils::LcsAnchorProof &proof = abTokAnchorProofs_[aToken];
      if (proof.kind !=
          diffutils::LcsAnchorProofKind::EquivalentNormalizedHunkAndOwner)
        continue;
      if (!alignmentSemanticTheoremActive_) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "A-token anchor {0} has semantic authority while the theorem "
            "boundary is inactive",
            aToken);
      }
      if (!semanticWitnessIds.count(proof.semanticWitnessId)) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "A-token anchor {0} names missing semantic witness {1}", aToken,
            proof.semanticWitnessId);
      }
      const auto witnessIt = llvm::find_if(
          alignmentSemanticResolutionWitnesses_,
          [&](const AlignmentSemanticResolutionWitness &witness) {
            return witness.witnessId == proof.semanticWitnessId;
          });
      if (witnessIt == alignmentSemanticResolutionWitnesses_.end() ||
          aToken >= witnessIt->representativeMap.size() ||
          witnessIt->representativeMap[aToken] != abTokMapA2B_[aToken]) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "A-token anchor {0} disagrees with semantic witness {1}", aToken,
            proof.semanticWitnessId);
      }
    }
  }

  std::vector<diffutils::Hunk> hunks = std::move(diffPlan.hunks);
  structuralHunkPlanningPhase_ =
      StructuralHunkPlanningPhase::InitialTokenDiffBuilt;

  REFOLD_LOG_DEBUG(
      "plan",
      "refold model loaded: tu={0} includes={1} macroInvocations={2} "
      "ppTokenMapEntries={3}",
      tuPath, model_.GetIncludes().size(), model_.GetMacroInvocations().size(),
      model_.GetTokmapByPP().size());

  // 2) Normalize the initial hunk sequence before any owner-sensitive state is
  // built.  The generalized tiler may lower one structural deletion to several
  // ordinary token hunks, but downstream services continue to consume only the
  // normalized vector and its durable segment bindings.
  assert(mixedOwnerTilingPlanner_ &&
         "structural tiling planner service not initialized");
  RefoldMixedOwnerTilingPlanner::MixedOwnerTilingPlan structuralTilingPlan =
      mixedOwnerTilingPlanner_->Plan(std::move(hunks));
  hunks = std::move(structuralTilingPlan.hunks);

  // The planner writes the shared token-hunk cache and durable ledgers as part
  // of the same normalization transaction.  Validate that transaction before
  // insertion provenance records hunk indices; after that point a mismatch
  // could redirect claims or owner dispatch to the wrong segment.
  std::string structuralLedgerFailure;
  const bool structuralLedgersAgree =
      structuralTilingLedgersMatchNormalizedHunks(
          hunks, mixedOwnerTilingWitnesses_,
          mixedOwnerTilingSegmentBindings_, structuralLedgerFailure);
  if (abTokHunks_ != hunks ||
      structuralTilingPlan.mixedOwnerWitnessCount !=
          mixedOwnerTilingWitnesses_.size() ||
      structuralTilingPlan.segmentBindingCount !=
          mixedOwnerTilingSegmentBindings_.size() ||
      !structuralLedgersAgree) {
    REFOLD_LOG_FATAL(
        "plan/order",
        "structural tiling did not atomically publish its normalized hunks "
        "and ledgers: returnedHunks={0} cachedHunks={1} "
        "reportedWitnesses={2} durableWitnesses={3} "
        "reportedBindings={4} durableBindings={5} detail={6}",
        hunks.size(), abTokHunks_.size(),
        structuralTilingPlan.mixedOwnerWitnessCount,
        mixedOwnerTilingWitnesses_.size(),
        structuralTilingPlan.segmentBindingCount,
        mixedOwnerTilingSegmentBindings_.size(), structuralLedgerFailure);
  }
  structuralHunkPlanningPhase_ =
      StructuralHunkPlanningPhase::StructuralTilingComplete;

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
          "deletions={3} structuralTilingWitnesses={4}",
          hunks.size(), replaceHunks, insertOnlyHunks, deleteOnlyHunks,
          mixedOwnerTilingWitnesses_.size());
  }

  // 3) Build provenance from the normalized sequence only.  In particular, a
  // structural deletion lowered to A=[a0,a1), B=[q,q) segments must not leave
  // insertion or claim indices referring to the original unsplit hunk.
  if (structuralHunkPlanningPhase_ !=
      StructuralHunkPlanningPhase::StructuralTilingComplete) {
    REFOLD_LOG_FATAL("plan/order",
                     "insertion provenance requested before structural "
                     "tiling completed");
  }
  BInsertionLedger().BuildProvenance(hunks);
  BInsertionLedger().PreclaimStandaloneInsertions(tuPath, hunks);
  structuralHunkPlanningPhase_ =
      StructuralHunkPlanningPhase::InsertionLedgerReady;
  return hunks;
}

void RefoldEngine::TraceStructuralHunkEnvelopes(
    ArrayRef<diffutils::Hunk> hunks) {
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
      REFOLD_LOG_FATAL(
          "hunks", "#{0} {1:verbose} B=OUT-OF-BOUNDS: h.bEnd={2} map.size={3}",
          i, h, h.bEnd, bTokOff_.size());
      continue;
    }

    const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];
    const size_t b1 = bTokOff_[static_cast<size_t>(h.bEnd)];

    // Case C: The token-to-byte map contains sentinels (virtual/synthetic
    // tokens)
    if (b0 == StringRef::npos || b1 == StringRef::npos) {
      REFOLD_LOG_FATAL("hunks", "#{0} {1:verbose} B=SENTINEL: b0={2} b1={3}", i,
                       h, (b0 == StringRef::npos ? "npos" : "valid"),
                       (b1 == StringRef::npos ? "npos" : "valid"));
      continue;
    }

    // Case D: Byte offsets are inverted (corrupt map or out-of-order tokens)
    if (b1 < b0) {
      REFOLD_LOG_FATAL("hunks",
                       "#{0} {1:verbose} B=INVERTED-OFFSETS: b0={2} b1={3}", i,
                       h, b0, b1);
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
}

bool RefoldEngine::StageSidebandEdits(
    StringRef tuPath, StringRef tuBytes,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  // Sideband pragmas are staged before ordinary structural dispatch so later
  // TU/include/macro realization can strip or avoid any separately-owned
  // sideband replay bytes from ordinary hunk replacements.
  return appendSidebandPragmaSourceEdits(
      sidebandPragmaEdits_, tuPath, tuBytes, pathIdentity_, *textEditAssembler_,
      ProofLattice(), terminalSink_, structuralHunkDispatcher);
}

bool RefoldEngine::DispatchStructuralHunks(
    StringRef tuPath, StringRef tuBytes, ArrayRef<diffutils::Hunk> hunks,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  // Owner classification, direct TU planning, and macro/include selection must
  // all observe exactly the hunk sequence used to build insertion provenance.
  // This guard prevents a future orchestration change from dispatching the
  // initial pre-tiling diff or from bypassing insertion-ledger construction.
  const bool dispatchMatchesNormalizedCache =
      hunks.size() == abTokHunks_.size() &&
      std::equal(hunks.begin(), hunks.end(), abTokHunks_.begin());
  if (structuralHunkPlanningPhase_ !=
          StructuralHunkPlanningPhase::InsertionLedgerReady ||
      !dispatchMatchesNormalizedCache) {
    REFOLD_LOG_FATAL(
        "plan/order",
        "structural owner dispatch observed an uncommitted hunk plan: "
        "phase={0} dispatchedHunks={1} cachedHunks={2}",
        static_cast<unsigned>(structuralHunkPlanningPhase_), hunks.size(),
        abTokHunks_.size());
  }

  // Local lexical predicate used by the theorem-lattice tie-breaker below.
  // A top-level comma in replacement text would split the original invocation
  // argument list, so the otherwise-equivalent TU edit must stay behind the
  // macro reconstruction proof path.
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
        ProofLattice().AcceptedCandidateBuilder().BuildAcceptedMacroCandidate(
            macroCandidate);
    if (!ProofLattice()
             .AcceptedResultRanker()
             .IsSelectableAcceptedResultCandidate(macroAccepted))
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
    if (!spanPlan || spanPlan->tuByteBegin >= spanPlan->tuByteEnd)
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

    AcceptedResultCandidate tuCandidate =
        ProofLattice()
            .AcceptedCandidateBuilder()
            .BuildAcceptedTUTextEditCandidate(
                AcceptedPathKind::TUByteSpanMappedEdit, hunk, *spanPlan,
                /*structuralBinding=*/nullptr, replacement);
    tuCandidate.proofSummary.selectionTieBreaker =
        TheoremSelectionTieBreakerKind::
            ExactTUArgumentEditOverEquivalentMacroArgsOnly;

    const bool latticePrefersTU =
        ProofLattice()
            .AcceptedResultRanker()
            .IsSelectableAcceptedResultCandidate(tuCandidate) &&
        ProofLattice().AcceptedResultRanker().LatticePrefers(
            tuCandidate.proofSummary, macroAccepted.proofSummary) &&
        !ProofLattice().AcceptedResultRanker().LatticePrefers(
            macroAccepted.proofSummary, tuCandidate.proofSummary);
    if (!latticePrefersTU)
      return false;

    return true;
  };

  // Iterate over all hunks:
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // These shape predicates are semantic control-flow inputs, not trace
    // payload. Keep them close to the dispatch decision that consumes them.
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
      macroTarget = macroBoundarySelector_.RightBoundaryVaOptActivationMacro(
          h.aStart, owner.includeId);
    if (!macroTarget && isIns)
      macroTarget = macroBoundarySelector_.BoundaryGeneratedSelectorMacro(
          h.aStart, owner.includeId);
    if (!macroTarget && isIns)
      macroTarget = macroBoundarySelector_.BoundaryDefinitionTapeReplayMacro(
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
          existingPatchContext.patch = stagingSlot.existingPatch;
          existingPatchContext.isCallsite = stagingSlot.existingIsCallsite;

          // Try to build a whole-cover replacement at the current target
          // invocation. If successful, install/replace the callsite patch for
          // this macro id and stop climbing the caller chain.
          auto updated =
              MacroPatchPlanner().BuildMacroInvocationPatchWholeCover(
                  *target, h, stagingSlot.currentInvocationText,
                  structuralHunkDispatcher.MacroPatchMergeBucketsForPlanner(),
                  existingPatchContext);
          if (updated) {
            if (!stagingSlot.existingPatch &&
                theoremLatticePrefersExactTUArgEditOverMacroArgsOnly(
                    *target, h, *updated)) {
              break;
            }

            const Owner currentPatchOwner =
                MacroPatchPlanner().NormalizeHunkOwnerForPatch(tuPath, h);
            if (stagingSlot.existingPatch)
              MacroPatchPlanner().CarryMacroPatchOwnerCertificate(
                  *updated, *stagingSlot.existingPatch);
            structuralHunkDispatcher.MergeMaterializedBTokenRangeFromSlot(
                *updated, stagingSlot, h);
            MacroPatchPlanner().CertifyMacroPatchOwnerWitness(
                *updated, currentPatchOwner);
            updated->macroId = stagingSlot.patchKey;
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
        parentBoundaryInc = model_.GetIncludeById(boundaryPlan->includeId);
      if (parentBoundaryInc) {
        IncludePatch patch =
            IncludeInsertionPlanner().BuildIncludeInsertionPatch(
                *parentBoundaryInc, h);
        patch.condArm.present = owner.condArmId.has_value();
        patch.condArm.armId = owner.condArmId.value_or(0);
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
          patch.condArm.present = owner.condArmId.has_value();
          patch.condArm.armId = owner.condArmId.value_or(0);
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
      patch.condArm.present = owner.condArmId.has_value();
      patch.condArm.armId = owner.condArmId.value_or(0);
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
                  (span.first > 0 && (tuBytes[span.first - 1] == ' ' ||
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
          // B attaches the inserted content to its left neighbour iff there is
          // no whitespace before the first inserted B token.
          const size_t bStartIdx = static_cast<size_t>(h.bStart);
          const bool replayAttachesLeftInB =
              bStartIdx < bTokOff_.size() && bTokOff_[bStartIdx] > 0 &&
              !stringutils::isWs(bSource_[bTokOff_[bStartIdx] - 1]);
          consumedSeparatorGapForPunctuation =
              maybeConsumeLeftSourceGapWhenBAttaches(
                  model_, pathIdentity_, tuPath, tuBytes, span, StringRef(repl),
                  replayAttachesLeftInB, lexLang_);
        }

        TUEditPlanner().MaybeExtendTUSpanOverClosedTrailingCallSuffix(
            h, tuPath, tuBytes, repl, span);

        bool advancedOverSourceLineControlPrefix =
            maybeAdvanceTUInsertionPastSourceLineControlPrefix(
                TUEditPlanner(), lineControlProof_, h, tuPath, tuBytes, span);
        std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment;
        if (advancedOverSourceLineControlPrefix) {
          insertionAnchorAdjustment = TUInsertionAnchorAdjustment{
              TUInsertionAnchorAdjustmentKind::SourceLineControlPrefix,
              rawTUStart, span.first};
        }

        // Is this span replacing a TU "gap" (bytes that are all whitespace)?
        std::string original;
        if (span.second > span.first) {
          original.assign(tuBytes.data() + span.first,
                          tuBytes.data() + span.second);
        } else if (span.second < span.first) {
          REFOLD_LOG_FATAL("tu/span", "invalid TU byte span: [{0},{1})",
                           span.first, span.second);
        }

        bool replacingGap = !original.empty() && stringutils::isWs(original);

        // If we’re replacing a non-empty TU gap and the inserted text doesn’t
        // start with WS, prefix EXACTLY ONE space from the gap to preserve
        // “return injected” (no double spaces).
        if (replacingGap && !consumedSeparatorGapForPunctuation &&
            !repl.empty() && !stringutils::isWs(repl.front())) {
          repl.insert(repl.begin(), ' ');
        }

        // Final boundary spacing fixup:
        // - On the left, only let refoldPadAtBoundaries add a space if we did
        //   not already preserve whitespace from a replaced TU gap; otherwise
        //   we could duplicate spacing.
        // - On the right, always allow padding if the replacement would
        //   otherwise glue to the following TU text.
        // Keep a copy for logging; refoldPadAtBoundaries consumes via move.
        std::string rawRepl = repl;

        std::string padded = refoldPadAtBoundaries(
            tuBytes, static_cast<size_t>(span.first),
            static_cast<size_t>(span.second), std::move(repl),
            /*allowLeft*/ !replacingGap, /*allowRight*/ true, lexLang_);

        const bool skipLocalResync =
            tuInsertionBeforeMaterializedInclude(
                TUEditPlanner(), model_, sidebandPragmaEdits_, h, tuPath, span,
                /*requireVisibleReplayText=*/true) ||
            lineControlProof_.TUInsertionCanDeferResyncToConditionalJoin(
                advancedOverSourceLineControlPrefix, tuPath, span.second);

        ResyncOutcome ro =
            skipLocalResync
                ? ResyncOutcome(padded, std::nullopt)
                : textEditAssembler_->ApplyResyncOrPend(
                      tuBytes, span.first, span.second, padded, tuPath);
        if (std::optional<TextEdit> directEdit =
                textEditAssembler_->BuildDirectTUHunkTextEdit(
                    h, i, span, std::move(ro), StringRef(padded), rawTUStart,
                    rawTUEnd, materializedBByteBegin, materializedBByteEnd,
                    AcceptedPathKind::TUByteSpanMappedEdit,
                    std::move(insertionAnchorAdjustment))) {
          structuralHunkDispatcher.AddTUEdit(std::move(*directEdit));
          continue;
        }
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
              expansionFallbackPlanner_
                  ->BuildTUIncludeClosureEditForUnresolvedHunk(
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
          ProofLattice().BuildOwnerUnresolvedNoTUAnchorDetail(i, h, tuPath,
                                                              owner, mapsToTU));
      continue;
    }

    if (auto spanPlan = TUEditPlanner().PlanTUByteSpan(h.aStart, h.aEnd,
                                                       tuPath)) { // [b, e)
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
            envelopeBegin =
                static_cast<uint64_t>(bSlice.data() - bSource_.data());
            envelopeEnd = *envelopeBegin + static_cast<uint64_t>(bSlice.size());
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
        // B attaches the inserted content to its left neighbour iff there is no
        // whitespace before the first inserted B token.
        const size_t bStartIdx = static_cast<size_t>(h.bStart);
        const bool replayAttachesLeftInB =
            bStartIdx < bTokOff_.size() && bTokOff_[bStartIdx] > 0 &&
            !stringutils::isWs(bSource_[bTokOff_[bStartIdx] - 1]);
        consumedSeparatorGapForPunctuation =
            maybeConsumeLeftSourceGapWhenBAttaches(
                model_, pathIdentity_, tuPath, tuBytes, span, StringRef(repl),
                replayAttachesLeftInB, lexLang_);
      }

      TUEditPlanner().MaybeExtendTUSpanOverClosedTrailingCallSuffix(
          h, tuPath, tuBytes, repl, span);

      bool advancedOverSourceLineControlPrefix =
          maybeAdvanceTUInsertionPastSourceLineControlPrefix(
              TUEditPlanner(), lineControlProof_, h, tuPath, tuBytes, span);
      std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment;
      if (advancedOverSourceLineControlPrefix) {
        insertionAnchorAdjustment = TUInsertionAnchorAdjustment{
            TUInsertionAnchorAdjustmentKind::SourceLineControlPrefix,
            rawTUStart, span.first};
      }

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
        REFOLD_LOG_FATAL("tu/span", "invalid TU byte span: [{0},{1})",
                         span.first, span.second);
      }

      // If this patch replaces a non-empty whitespace gap in the TU, and the
      // replacement text does not already begin with whitespace, prefix a
      // single space so adjacent tokens remain separated. Add only one space,
      // even if the original gap was wider, to avoid duplicating spacing.
      if (replacingGap && !consumedSeparatorGapForPunctuation &&
          !repl.empty() && !stringutils::isWs(repl.front()))
        repl.insert(repl.begin(), ' ');

      // Keep a copy for logging; refoldPadAtBoundaries consumes via move.
      std::string rawRepl = repl;

      std::string padded = refoldPadAtBoundaries(
          tuBytes, static_cast<size_t>(span.first),
          static_cast<size_t>(span.second), std::move(repl),
          /*allowLeft*/ !replacingGap, /*allowRight*/ true, lexLang_);

      const bool skipLocalResync =
          tuInsertionBeforeMaterializedInclude(
              TUEditPlanner(), model_, sidebandPragmaEdits_, h, tuPath, span,
              /*requireVisibleReplayText=*/false) ||
          lineControlProof_.TUInsertionCanDeferResyncToConditionalJoin(
              advancedOverSourceLineControlPrefix, tuPath, span.second);

      ResyncOutcome ro =
          skipLocalResync
              ? ResyncOutcome(padded, std::nullopt)
              : textEditAssembler_->ApplyResyncOrPend(
                    tuBytes, span.first, span.second, padded, tuPath);
      if (std::optional<TextEdit> directEdit =
              textEditAssembler_->BuildDirectTUHunkTextEdit(
                  h, i, span, std::move(ro), StringRef(padded), rawTUStart,
                  rawTUEnd, materializedBByteBegin, materializedBByteEnd,
                  AcceptedPathKind::TUByteSpanConservativeEdit,
                  std::move(insertionAnchorAdjustment))) {
        structuralHunkDispatcher.AddTUEdit(std::move(*directEdit));
        continue;
      }
    }

    // Before we escalate to the explicit terminal out-of-domain carrier, try
    // the declared TUIncludeClosureEdit class.  This source-closure proof keeps
    // already-proved structural work alive by materializing a closed run of
    // top-level TU `#include` directives directly into TU source text when that
    // run either exactly covers the unresolved PP hunk or can be widened to the
    // full include cover without absorbing another token diff.
    llvm::SmallVector<std::pair<uint64_t, uint64_t>, 8> stagedSourceIntervals =
        structuralHunkDispatcher.BuildTUClosureSourceIntervals();
    if (auto closureEdit = expansionFallbackPlanner_
                               ->BuildTUIncludeClosureEditForUnresolvedHunk(
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
        ProofLattice().BuildOwnerUnresolvedNoTUAnchorDetail(i, h, tuPath, owner,
                                                            mapsToTU));
    continue;
  }

  return true;
}

std::string RefoldEngine::FinalizeStructuralResult(
    StringRef tuPath, StringRef tuBytes, ArrayRef<diffutils::Hunk> hunks,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  std::vector<TextEdit> &tuEdits =
      structuralHunkDispatcher.MutableTUEditsForRepairAndEmission();
  // Global fail-closed composition rule: once this single structural pass has
  // requested terminal fallback, do not continue composing structural
  // artifacts. The outer driver will discard the current attempt and emit B
  // directly.
  if (terminalSink_.HasRequest()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted after classification; "
                     "terminal fallback will be emitted");
    return std::string();
  }

  // Repair macro-state liveness after TU-level source edits.  The planner
  // owns the directive index, final TU edit interval queries, include ancestry
  // checks, proof witnesses, and conservative TU edit mutations used by all
  // macro-state repair phases.
  RefoldMacroStateRepairPlanner::MacroStateRepairRequest
      macroStateRepairRequest{tuPath, tuBytes, &structuralHunkDispatcher,
                              &tuEdits};
  RefoldMacroStateRepairPlanner::MacroStateRepairPlan macroStateRepairPlan =
      MacroStateRepairPlanner().Plan(macroStateRepairRequest);
  if (!macroStateRepairPlan.success)
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
    applyForcedCounterPatches(forcedCounters, bToks_, sourceMapper_,
                              macroTopology_, MacroPatchPlanner(),
                              ProofLattice(), OwnerStateProof(),
                              structuralHunkDispatcher);

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
      if (text.starts_with("#line") || (text.size() >= 2 && text[0] == '#' &&
                                        stringutils::isNonNewlineWs(text[1])))
        return true;
    }

    return false;
  };

  auto replayContextSensitivePredefinedBuiltin =
      [&](StringRef name, const RefoldModel::MacroInvocation &site) {
        // These builtins produce replay-time or physical-file-timestamp
        // literals that cannot be repaired by synthetic #line state.
        if (name == "__TIMESTAMP__" || name == "__DATE__" || name == "__TIME__")
          return true;

        if (name != "__BASE_FILE__")
          return false;

        // __BASE_FILE__ is normally part of the line/file observer model: a
        // preserved source-authored #line can make the spelling replay to the
        // same producer-observed value, and forcing B realization in those
        // cases destroys better source-preserving refoldings.  The remaining
        // unsafe case is a value-producing TU-local spelling that would
        // otherwise require a synthetic prologue solely to hide the checker
        // output path.  Prefer the local producer-proven literal for that
        // single observer instead of adding a global line directive at the top
        // of the refolded file.
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
      if (!lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(
              builtin))
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
      //     would otherwise need a synthetic prologue to mask the checker
      //     output path.
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
      if (stagingSlot.existingPatch)
        MacroPatchPlanner().CarryMacroPatchOwnerCertificate(
            patch, *stagingSlot.existingPatch);
      ProofLattice().CertifyMacroWholeCoverRealizationPatch(patch, *plan,
                                                            *site);
      MacroPatchPlanner().CertifyMacroPatchOwnerWitness(
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
  // planning passes. The dispatcher owns the DenseMap merge buckets and
  // flattens them deterministically before final include/TU emission.
  structuralHunkDispatcher.FinalizeMacroPatchBuckets(ProofLattice());

  if (!structuralHunkDispatcher.AppendLineObserverRealizationEdits(
          LineObserverLayout(), tuPath, tuBytes))
    return std::string();

  // Normalize/coalesce include-side insertions.
  structuralHunkDispatcher.OrderIncludeInsertions();

  // 5) Schedule include materializations.  The scheduler owns the run-local
  // child index, materialization seed set, realized expansion cache, and
  // source-graph/line-control metadata shared by the early materialization
  // include-materialization path and the later TU-root include edit emission
  // path.
  RefoldIncludeMaterializationScheduler::Dependencies includeSchedulerDeps;
  includeSchedulerDeps.model = &model_;
  includeSchedulerDeps.pathIdentity = &pathIdentity_;
  includeSchedulerDeps.includeMaterializer = includeMaterializer_.get();
  includeSchedulerDeps.includeInsertionPlanner = includeInsertionPlanner_.get();
  includeSchedulerDeps.lineObserverLayout = lineObserverLayout_.get();
  includeSchedulerDeps.macroStateRepairPlanner = macroStateRepairPlanner_.get();
  includeSchedulerDeps.textEditAssembler = textEditAssembler_.get();
  includeSchedulerDeps.proofLattice = proofLattice_.get();
  includeSchedulerDeps.pragmaOnceGuards = pragmaOnceGuardRewriter_.get();
  includeSchedulerDeps.terminalSink = &terminalSink_;
  includeSchedulerDeps.sidebandPragmaEdits = &sidebandPragmaEdits_;

  RefoldIncludeMaterializationScheduler::IncludeMaterializationRequest
      includeSchedulerRequest;
  includeSchedulerRequest.tuPath = tuPath;
  includeSchedulerRequest.tuBytes = tuBytes;
  includeSchedulerRequest.aSource = aSource_;
  includeSchedulerRequest.bSource = bSource_;
  includeSchedulerRequest.aTokens = aToks_;
  includeSchedulerRequest.bTokens = bToks_;
  includeSchedulerRequest.aTokenOffsets = aTokOff_;
  includeSchedulerRequest.bTokenOffsets = bTokOff_;
  includeSchedulerRequest.tokenHunks = hunks;
  includeSchedulerRequest.rawByteHunks =
      abByteHunks_ ? &*abByteHunks_ : nullptr;
  includeSchedulerRequest.structuralHunkDispatcher = &structuralHunkDispatcher;
  includeSchedulerRequest.sourceGraphOutputs = sourceGraphOutputs_;

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
  finalEmissionDeps.model = &model_;
  finalEmissionDeps.macroTopology = &macroTopology_;
  finalEmissionDeps.lineControlProof = &lineControlProof_;
  finalEmissionDeps.lineDirs = &lineDirs_;
  finalEmissionDeps.macroStateRepairPlanner = macroStateRepairPlanner_.get();
  finalEmissionDeps.textEditAssembler = textEditAssembler_.get();
  finalEmissionDeps.proofLattice = proofLattice_.get();
  finalEmissionDeps.terminalSink = &terminalSink_;
  finalEmissionDeps.materializedEditMappings = materializedEditMappings_;
  finalEmissionDeps.finalLineControlPruneCandidates =
      &finalLineControlPruneCandidates_;
  finalEmissionDeps.finalLineControlSourceMappings =
      &finalLineControlSourceMappings_;

  RefoldFinalTUEmissionPlanner::EmissionRequest finalEmissionRequest;
  finalEmissionRequest.tuPath = tuPath;
  finalEmissionRequest.tuBytes = tuBytes;
  finalEmissionRequest.structuralHunkDispatcher = &structuralHunkDispatcher;
  finalEmissionRequest.includeMaterializationScheduler =
      &includeMaterializationScheduler;
  finalEmissionRequest.macroStatePlan = &macroStateRepairPlan;
  finalEmissionRequest.macroStateRequest = &macroStateRepairRequest;

  RefoldFinalTUEmissionPlanner finalEmissionPlanner(
      std::move(finalEmissionDeps));
  RefoldFinalTUEmissionPlanner::EmissionResult finalEmission =
      finalEmissionPlanner.PlanAndEmit(finalEmissionRequest);
  if (!finalEmission.success)
    return std::string();

  // Capture the exact final staged TU/include/macro topology after every
  // final-emission lowering and repair has been applied. This is evidence-only
  // and is consumed only by isolated alignment simulations.
  AlignmentSemanticTopologyKeyResult topologyKey =
      structuralHunkDispatcher.BuildAlignmentSemanticTopologyKey(
          ProofLattice().EquivalenceKeyBuilder(), tuBytes);
  for (uint64_t includeId :
       includeMaterializationScheduler.ExpandedIncludeIds())
    topologyKey.preservationFootprint.expandedIncludeIds.push_back(includeId);
  for (uint64_t macroRootId :
       structuralHunkDispatcher.ExpandedMacroRootIds())
    topologyKey.preservationFootprint.expandedMacroRootIds.push_back(
        macroRootId);
  llvm::sort(topologyKey.preservationFootprint.expandedIncludeIds);
  llvm::sort(topologyKey.preservationFootprint.expandedMacroRootIds);
  alignmentSimulationStagedTopologyComplete_ = topologyKey.complete;
  alignmentSimulationStagedTopologyFailure_ = std::move(topologyKey.failure);
  alignmentSimulationStagedTopologyKey_ = std::move(topologyKey.key);
  alignmentSimulationPreservationFootprint_ =
      std::move(topologyKey.preservationFootprint);

  lastStats_.expandedMacros = finalEmission.expandedMacroCount;
  return std::move(finalEmission.tuText);
}

std::string RefoldEngine::RunRefoldPass() {
  if (!ValidateTokenCount())
    return std::string();

  StringRef tuPath = model_.GetSourcePath();
  REFOLD_LOG_INFO(
      "plan",
      "starting refold: tu={0} ppBytes={1} ppModBytes={2} ppTokens={3} "
      "ppModTokens={4}",
      tuPath, aSource_.size(), bSource_.size(), aToks_.size(), bToks_.size());

  // Reset the structural planning phase and run-local token diff caches before
  // any stage repopulates them for this pass.  PlanTokenDiff() advances the
  // phase monotonically through initial diff, tiling, and insertion-ledger
  // publication before DispatchStructuralHunks() is allowed to run.
  structuralHunkPlanningPhase_ = StructuralHunkPlanningPhase::NotStarted;
  abTokHunks_.clear();
  abTokMapA2B_.clear();
  abTokMapB2A_.clear();
  abTokAnchorProofs_.clear();
  alignmentSemanticResolutionWitnesses_.clear();
  alignmentSimulationStagedTopologyKey_.clear();
  alignmentSimulationStagedTopologyComplete_ = true;
  alignmentSimulationStagedTopologyFailure_.clear();
  alignmentSimulationPreservationFootprint_ =
      AlignmentSemanticPreservationFootprint{};
  alignmentSemanticTheoremActive_ = alignmentSelectionOverride_.has_value();

  std::unique_ptr<llvm::MemoryBuffer> tuBuffer = LoadTUSource(tuPath);
  StringRef tuBytes = tuBuffer->getBuffer();

  std::vector<diffutils::Hunk> hunks = PlanTokenDiff(tuPath);
  TraceStructuralHunkEnvelopes(hunks);

  RefoldStructuralHunkDispatcher structuralHunkDispatcher;
  if (!StageSidebandEdits(tuPath, tuBytes, structuralHunkDispatcher))
    return std::string();
  if (!DispatchStructuralHunks(tuPath, tuBytes, hunks,
                                structuralHunkDispatcher))
    return std::string();

  return FinalizeStructuralResult(tuPath, tuBytes, hunks,
                                  structuralHunkDispatcher);
}

} // namespace refold
} // namespace clang

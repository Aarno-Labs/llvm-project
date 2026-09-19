//===--- RefoldServiceGraphBuilder.cpp --------------------------*- C++ -*-===//
//
// Centralized RefoldEngine service-graph construction.
//
// The composition root: RefoldEngine::BuildServiceGraph() constructs every
// planning, proof and emission service in dependency order, each from explicit
// inputs and services.  Subsystem .cpp files implement subsystem behavior; this
// file owns only the object-graph wiring and its order.  The remaining hook
// bundles are listed in docs/CallbackInventory.md.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldEngine.h"

#include "edit/RefoldBInsertionLedger.h"
#include "edit/RefoldExpansionFallbackPlanner.h"
#include "edit/RefoldTUAnchorProof.h"
#include "edit/RefoldTUEditPlanner.h"
#include "edit/RefoldTextEditAssembler.h"
#include "edit/RefoldTextEditCertifier.h"
#include "include/RefoldIncludeInsertionPlanner.h"
#include "include/RefoldIncludeMaterializer.h"
#include "include/RefoldPragmaOnceGuardRewriter.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldCounterStabilization.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroStateRepairPlanner.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroWholeCoverPlanBuilder.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofServices.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldOwnerClassifier.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldPreprocessingStructureIndexProvider.h"
#include "source/RefoldStructuralHunkTilingPlanner.h"
#include "source/RefoldTokenDiffPlanner.h"
#include "support/RefoldLog.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Derive the source carrier of every paired `#pragma` line.
///
/// The producer record printed into the line names its source: a directive's
/// site, or a `_Pragma` operator's exact bytes.  When a macro expansion printed
/// the line, the source that survives or not is the root invocation's
/// spelling instead; a caller chain that does not resolve falls back to the
/// record's own site, which the producer places at the invocation.  A line
/// whose record does not bind uniquely yields no carrier, which leaves it to
/// the planner's placement check alone.
std::vector<PrintedPragmaCarrier>
buildPrintedPragmaCarriers(const RefoldModel &model,
                           const RefoldMacroTopology &topology,
                           ArrayRef<SidebandPragmaLinePairing> pairings) {
  std::vector<PrintedPragmaCarrier> carriers;
  for (const SidebandPragmaLinePairing &line : pairings) {
    if (!line.bNormalTokenGap)
      continue;
    const RefoldModel::PragmaDirective *record = nullptr;
    bool unique = true;
    for (const RefoldModel::PragmaDirective &pragma : model.GetPragmas()) {
      if (!pragma.HasEmittedImage() || *pragma.ppByteBegin < line.aLineBegin ||
          line.aLineEnd < *pragma.ppByteEnd)
        continue;
      unique = record == nullptr;
      record = &pragma;
    }
    if (!record || !unique)
      continue;

    const std::optional<ArrayRef<uint64_t>> ancestors =
        topology.PragmaExpansionAncestorsAtGap(line.aNormalTokenGap);
    if (ancestors && !ancestors->empty()) {
      for (uint64_t id : *ancestors) {
        const RefoldModel::MacroInvocation *root =
            topology.FindMacroInvocationById(id);
        if (!root || root->callerMacroId || !root->invFile || !root->invB ||
            !root->invE)
          continue;
        carriers.push_back({*root->invFile, root->ownerIncludeId, *root->invB,
                            *root->invE, line.bLineBegin, line.bLineEnd});
      }
      continue;
    }
    const bool operatorBytes = ancestors && record->viaPragmaOperator &&
                               record->operatorB && record->operatorE;
    carriers.push_back({record->sitePath, record->ownerIncludeId,
                        operatorBytes ? *record->operatorB : record->siteB,
                        operatorBytes ? *record->operatorE : record->siteE,
                        line.bLineBegin, line.bLineEnd});
  }
  return carriers;
}

} // namespace

void RefoldEngine::BuildServiceGraph() {
  // The audit is built before the proof services, which take it by reference.
  // It borrows only the proof-summary builder, which depends on B alone and is
  // therefore built first; every other fact it audits is supplied by the
  // caller that holds it.
  proofSummaryBuilder_ = std::make_unique<RefoldProofSummaryBuilder>(
      RefoldProofSummaryBuilder::Dependencies{bToks_});
  theoremAudit_ = std::make_unique<RefoldTheoremAudit>(
      lastTheoremAudit_, terminalSink_, *proofSummaryBuilder_, strict_,
      alignmentSemanticTheoremActive_);

  ownerStateProof_ = std::make_unique<RefoldOwnerStateProof>(
      RefoldOwnerStateProofInputs{model_, aToks_, bToks_, lexLang_,
                                  ownerStateGraphMemo_},
      pathIdentity_, tokenTextAnalysis_, macroTopology_, *theoremAudit_,
      terminalSink_);

  macroStateProof_ = std::make_unique<RefoldMacroStateProof>(
      model_, pathIdentity_, tokenTextAnalysis_, *ownerStateProof_);

  if (tuSourceBytesOverride_) {
    tuSourceLoadError_.reset();
    tuSourceBytes_ = *tuSourceBytesOverride_;
  } else {
    const std::string absoluteTUPath =
        lineDirs_.ToAbsolutePath(model_.GetSourcePath());
    auto bufferOrError = MemoryBuffer::getFile(absoluteTUPath);
    if (!bufferOrError) {
      tuSourceLoadError_ =
          llvm::formatv("unable to read translation unit '{0}': {1}",
                        absoluteTUPath, bufferOrError.getError().message())
              .str();
      tuSourceBytes_.clear();
      REFOLD_LOG_WARN("tu/structure-index",
                      "{0}; direct TU spans will fail closed",
                      *tuSourceLoadError_);
    } else {
      tuSourceLoadError_.reset();
      tuSourceBytes_ = bufferOrError.get()->getBuffer().str();
    }
  }

  preprocessingStructureIndex_ =
      std::make_unique<RefoldPreprocessingStructureIndex>(
          RefoldPreprocessingStructureIndex::Build(
              RefoldPreprocessingStructureIndex::Dependencies{
                  model_, pathIdentity_, lexLang_},
              model_.GetSourcePath(), tuSourceBytes_, std::nullopt));

  // Protection diagnostics invalidate the whole physical census and therefore
  // reject every direct TU byte span.  Ordinary producer-binding diagnostics
  // are local: the scanned interval remains protected, but an unrelated
  // mismatch elsewhere in the file must not change owner classification or
  // macro replay ranking.
  for (StringRef diagnostic :
       preprocessingStructureIndex_->GetDirectTUProtectionDiagnostics()) {
    REFOLD_LOG_WARN("tu/structure-index",
                    "incomplete direct-TU protection census: {0}", diagnostic);
  }
  for (StringRef diagnostic : preprocessingStructureIndex_->GetDiagnostics()) {
    REFOLD_LOG_DEBUG("tu/structure-index", "index diagnostic: {0}", diagnostic);
  }

  preprocessingStructureIndexProvider_ =
      std::make_unique<RefoldPreprocessingStructureIndexProvider>(
          RefoldPreprocessingStructureIndexProvider::Dependencies{
              model_, pathIdentity_, lineDirs_, lexLang_},
          model_.GetSourcePath(), *preprocessingStructureIndex_);

  // Token diff planning borrows the per-run source/token inputs and writes the
  // engine-owned diff caches later services observe.  The named service owns
  // lexeme/LCS provenance construction while preserving cache ownership and
  // lifetime on the engine object graph.
  tokenDiffPlanner_ = std::make_unique<RefoldTokenDiffPlanner>(
      RefoldTokenDiffPlanner::Dependencies{
          model_, pathIdentity_, *preprocessingStructureIndexProvider_,
          bSource_,
          aToks_, bToks_, aTokOff_, bTokOff_, macroTopology_, sourceMapper_,
          ownerDepthGap_, abTokHunks_, abByteHunks_, abTokMapA2B_,
          abTokMapB2A_, abTokAnchorProofs_,
          alignmentSelectionOverride_ ? &*alignmentSelectionOverride_ : nullptr,
          alignmentSemanticResolverEnabled_
              ? std::function<void(
                    ArrayRef<StringRef>, ArrayRef<StringRef>,
                    ArrayRef<diffutils::LcsAGapProvenance>,
                    ArrayRef<diffutils::LcsBGapProvenance>, uint64_t,
                    diffutils::CertifiedLcsResult &)>(
                    [this](ArrayRef<StringRef> aLexemes,
                           ArrayRef<StringRef> bLexemes,
                           ArrayRef<diffutils::LcsAGapProvenance>
                               aGapProvenance,
                           ArrayRef<diffutils::LcsBGapProvenance>
                               bGapProvenance,
                           uint64_t certificationByteBudget,
                           diffutils::CertifiedLcsResult &alignment) {
                      ResolveSemanticAlignment(aLexemes, bLexemes,
                                               aGapProvenance, bGapProvenance,
                                               certificationByteBudget,
                                               alignment);
                    })
              : std::function<void(
                    ArrayRef<StringRef>, ArrayRef<StringRef>,
                    ArrayRef<diffutils::LcsAGapProvenance>,
                    ArrayRef<diffutils::LcsBGapProvenance>, uint64_t,
                    diffutils::CertifiedLcsResult &)>()});

  // TU anchor and byte-span proofs.  Audit flows through the shared
  // theorem/audit service and summary construction through the shared
  // proof-summary builder.
  tuAnchorProof_ = std::make_unique<RefoldTUAnchorProof>(
      *theoremAudit_, RefoldTUAnchorProof::Deps{
                          model_, pathIdentity_, macroTopology_, lineDirs_,
                          *preprocessingStructureIndex_, tuSourceBytes_, aToks_,
                          static_cast<uint64_t>(bToks_.size()), bToks_});

  // TU edit planning has a real owner.  The planner receives only read-only
  // services and token-map state; final TextEdit assembly intentionally remains
  // outside this service.
  tuEditPlanner_ =
      std::make_unique<RefoldTUEditPlanner>(RefoldTUEditPlanner::Deps{
          model_, macroTopology_, *tuAnchorProof_, aToks_, abTokMapA2B_});

  // Owner classification depends on the TU anchor proofs as a named,
  // read-only dependency.  RefoldTUAnchorProof does not depend on the owner
  // classifier, so normal constructor injection keeps the TU-anchor/TU-span
  // boundary explicit without creating a service cycle.
  ownerClassifier_ =
      std::make_unique<RefoldOwnerClassifier>(RefoldOwnerClassifier::Deps{
          model_, pathIdentity_, *tuAnchorProof_, sidebandPragmaEdits_});

  // Structural tiling borrows the owner/proof services and exact preprocessing
  // census needed to prove deterministic token-hunk partitions.  The planner
  // atomically publishes the normalized token-hunk cache and durable witness
  // ledgers.  PlanTokenDiff() validates that publication before insertion
  // provenance or owner dispatch can observe hunk indices.
  structuralHunkTilingPlanner_ =
      std::make_unique<RefoldStructuralHunkTilingPlanner>(
          RefoldStructuralHunkTilingPlanner::Dependencies{
              model_, model_.GetSourcePath(), pathIdentity_, macroTopology_,
              *macroStateProof_, tokenTextAnalysis_, sourceMapper_,
              tuSourceBytes_, bSource_, sidebandPragmaLinePairings_,
              sidebandPragmaEdits_, lexLang_, *ownerClassifier_,
              *ownerStateProof_, *preprocessingStructureIndexProvider_,
              abTokHunks_, structuralHunkTilingWitnesses_,
              structuralHunkTilingSegmentBindings_});

  // Pure B-token insertion ownership is a named edit-domain ledger.  The
  // ledger borrows the owner classifier and macro-boundary selector directly;
  // standalone preclaims depend only on these explicit services and the
  // ledger's own run-local state.
  bInsertionLedger_ = std::make_unique<RefoldBInsertionLedger>(
      RefoldBInsertionLedger::Deps{bToks_, sourceMapper_, *ownerClassifier_,
                                   macroTopology_, macroBoundarySelector_});

  // Whole-cover plan computation reads only the A->B source mapper and the
  // B-insertion claim ledger.  It is deliberately constructed here, ahead of
  // the macro patch planner and the text-edit assembler, because both of those
  // consume plans.
  wholeCoverPlanBuilder_ = std::make_unique<RefoldMacroWholeCoverPlanBuilder>(
      RefoldMacroWholeCoverPlanBuilder::Dependencies{sourceMapper_,
                                                     *bInsertionLedger_});

  counterStabilization_ = std::make_unique<RefoldCounterStabilization>(
      model_, aToks_, bToks_, macroTopology_, *ownerClassifier_);

  // Every input is constructed before this point and passed by reference, so
  // no proof service is late-bound.
  proofServices_ = std::make_unique<RefoldProofServices>(
      model_, bSource_, bToks_, sourceMapper_, tokenTextAnalysis_,
      argTextRecovery_, macroTopology_, *ownerStateProof_, *tuAnchorProof_,
      *proofSummaryBuilder_, *theoremAudit_, lastTheoremAudit_, witnessTrace_,
      structuralHunkTilingSegmentBindings_, structuralHunkTilingWitnesses_);

  // The planner is constructed after the proof services it borrows.  The
  // dependency bundle is intentionally explicit: macro planning reads source
  // data and proof services directly rather than calling back into
  // RefoldEngine.
  RefoldMacroPatchPlanner::Dependencies patchDeps;
  patchDeps.model = &model_;
  patchDeps.bSource = bSource_;
  patchDeps.aToks = aToks_;
  patchDeps.bToks = bToks_;
  patchDeps.bTokOff = bTokOff_;
  patchDeps.abTokHunks = &abTokHunks_;
  patchDeps.bInsertionLedger = bInsertionLedger_.get();
  patchDeps.argTextRecovery = &argTextRecovery_;
  patchDeps.lexLang = &lexLang_;
  patchDeps.lineDirs = &lineDirs_;
  patchDeps.macroTopology = &macroTopology_;
  patchDeps.pathIdentity = &pathIdentity_;
  patchDeps.sourceMapper = &sourceMapper_;
  patchDeps.ownerClassifier = ownerClassifier_.get();
  patchDeps.wholeCoverPlanBuilder = wholeCoverPlanBuilder_.get();
  patchDeps.strict = strict_;
  patchDeps.ownersMustExpand = &ownersMustExpand_;
  patchDeps.sidebandPragmaLinePairings = sidebandPragmaLinePairings_;

  patchDeps.macroStateProof = macroStateProof_.get();
  patchDeps.ownerStateProof = ownerStateProof_.get();
  patchDeps.macroPatchProofClassifier =
      &proofServices_->MacroPatchProofClassifier();
  patchDeps.acceptedCandidateBuilder =
      &proofServices_->AcceptedCandidateBuilder();
  patchDeps.acceptedResultRanker = &proofServices_->AcceptedResultRanker();
  patchDeps.witnessTrace = &witnessTrace_;
  macroPatchPlanner_ =
      std::make_unique<RefoldMacroPatchPlanner>(std::move(patchDeps));

  includeInsertionPlanner_ = std::make_unique<RefoldIncludeInsertionPlanner>(
      bSource_, bToks_, bTokOff_, sourceMapper_,
      proofServices_->AcceptancePathClassifier());

  // Protected-source capabilities and materialization certification sit below
  // planning, so the planners, the layout and the assembler all borrow them.
  printedPragmaCarriers_ = buildPrintedPragmaCarriers(
      model_, macroTopology_, sidebandPragmaLinePairings_);
  textEditCertifier_ = std::make_unique<RefoldTextEditCertifier>(
      model_, bSource_, bToks_, sourceMapper_, pathIdentity_, lexLang_,
      *preprocessingStructureIndex_, *tuAnchorProof_, terminalSink_,
      *theoremAudit_, printedPragmaCarriers_);

  lineObserverLayout_ = std::make_unique<RefoldLineObserverLayout>(
      model_, bSource_, aToks_, bToks_, bTokOff_, abTokMapA2B_, abTokMapB2A_,
      pathIdentity_, lineControlProof_, *ownerStateProof_,
      proofServices_->AcceptedCandidateBuilder(), *textEditCertifier_,
      lineDirs_);

  // The assembler owns final byte-application behavior.  Theorem/audit policy
  // is injected as RefoldTheoremAudit, and newline-drift resync is the
  // layout's, built above.
  textEditAssembler_ = std::make_unique<RefoldTextEditAssembler>(
      model_, bSource_, aToks_, bToks_, bTokOff_, abTokHunks_, abTokMapA2B_,
      abTokMapB2A_, sourceMapper_, lexLang_, *proofSummaryBuilder_,
      proofServices_->AcceptedCandidateBuilder(),
      proofServices_->OwnerRealizationProofBuilder(), *wholeCoverPlanBuilder_,
      *ownerStateProof_, macroTopology_, lineControlProof_, lineDirs_,
      terminalSink_, *tuEditPlanner_, *tuAnchorProof_, *textEditCertifier_,
      *lineObserverLayout_, *theoremAudit_, structuralHunkTilingWitnesses_,
      lastTheoremAudit_);

  RefoldMacroStateRepairPlanner::Dependencies repairDeps;
  repairDeps.model = &model_;
  repairDeps.pathIdentity = &pathIdentity_;
  repairDeps.macroTopology = &macroTopology_;
  repairDeps.tokenTextAnalysis = &tokenTextAnalysis_;
  repairDeps.macroStateProof = macroStateProof_.get();
  repairDeps.ownerStateProof = ownerStateProof_.get();
  repairDeps.macroPatchProofClassifier =
      &proofServices_->MacroPatchProofClassifier();
  repairDeps.acceptedCandidateBuilder =
      &proofServices_->AcceptedCandidateBuilder();
  repairDeps.macroPatchPlanner = macroPatchPlanner_.get();
  repairDeps.textEditCertifier = textEditCertifier_.get();
  repairDeps.lineObserverLayout = lineObserverLayout_.get();
  repairDeps.terminalSink = &terminalSink_;
  repairDeps.lexLang = &lexLang_;
  macroStateRepairPlanner_ =
      std::make_unique<RefoldMacroStateRepairPlanner>(std::move(repairDeps));

  // The pragma-once catalog is built from producer include/pragma facts and
  // the physical header bytes, so it must follow the certifier and
  // accepted-candidate builder it authorizes and certifies edits through.
  pragmaOnceGuardRewriter_ = std::make_unique<RefoldPragmaOnceGuardRewriter>(
      RefoldPragmaOnceGuardRewriter::Dependencies{
          model_, pathIdentity_, lineDirs_, lineControlProof_,
          *textEditCertifier_, *lineObserverLayout_,
          proofServices_->AcceptedCandidateBuilder(), terminalSink_, lexLang_},
      RefoldPragmaOnceGuardRewriter::GuardNameInputs{
          model_.GetSourcePath(), tuSourceBytes_, aSource_, bSource_});

  // Include proof, topology and terminal services are named constructor
  // dependencies of the materializer.
  includeMaterializer_ = std::make_unique<RefoldIncludeMaterializer>(
      model_, aSource_, bSource_, aToks_, bToks_, bTokOff_, abTokMapA2B_,
      lineDirs_, finalReplaySurface_, sidebandPragmaEdits_, sourceMapper_,
      pathIdentity_, macroTopology_, lineControlProof_, *lineObserverLayout_,
      *macroStateProof_, *ownerStateProof_, *includeInsertionPlanner_,
      proofServices_->AcceptedCandidateBuilder(),
      proofServices_->AcceptedResultRanker(), *textEditAssembler_,
      *textEditCertifier_, *pragmaOnceGuardRewriter_, terminalSink_, lexLang_);

  // The fallback planner is allocated after the materialization, proof and
  // certification services it calls into.
  expansionFallbackPlanner_ = std::make_unique<RefoldExpansionFallbackPlanner>(
      model_, bSource_, aToks_, abTokHunks_, abTokMapB2A_, abTokAnchorProofs_,
      lineDirs_, sourceMapper_, pathIdentity_, macroTopology_,
      lineControlProof_, *macroStateProof_, *preprocessingStructureIndex_,
      terminalSink_, lexLang_, *includeInsertionPlanner_,
      proofServices_->AcceptedCandidateBuilder(), *theoremAudit_, lastStats_,
      materializedEditMappings_, *textEditCertifier_, *lineObserverLayout_);
}

} // namespace refold
} // namespace clang

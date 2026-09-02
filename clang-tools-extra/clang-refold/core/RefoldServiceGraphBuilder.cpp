//===--- RefoldServiceGraphBuilder.cpp --------------------------*- C++ -*-===//
//
// Centralized RefoldEngine service-graph construction.
//
// This translation unit intentionally contains the RefoldEngine::Initialize*()
// and service-accessor definitions.  Subsystem .cpp files implement subsystem
// behavior; this file owns the engine object-graph wiring and dependency order.
// Hooks remain only for explicit cycle-breaking or engine-owned emission
// ledgers; theorem/audit policy, owner/TU classification, and TU edit planning
// flow through named services.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldEngine.h"

#include "core/RefoldLog.h"
#include "core/RefoldOwnerClassifier.h"
#include "edit/RefoldBInsertionLedger.h"
#include "edit/RefoldExpansionFallbackPlanner.h"
#include "edit/RefoldTUAnchorProof.h"
#include "edit/RefoldTUEditPlanner.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/RefoldIncludeInsertionPlanner.h"
#include "include/RefoldIncludeMaterializer.h"
#include "include/RefoldPragmaOnceGuardRewriter.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldCounterStabilization.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroStateRepairPlanner.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldMixedOwnerTilingPlanner.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldPreprocessingStructureIndexProvider.h"
#include "source/RefoldTokenDiffPlanner.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroStateProof &RefoldEngine::MacroStateProof() {
  assert(macroStateProof_ && "macro-state proof service not initialized");
  return *macroStateProof_;
}

const RefoldMacroStateProof &RefoldEngine::MacroStateProof() const {
  assert(macroStateProof_ && "macro-state proof service not initialized");
  return *macroStateProof_;
}

void RefoldEngine::InitializeOwnerClassifier() {
  // Owner classification depends on the TU edit-planning service as a named,
  // read-only dependency.  RefoldTUEditPlanner does not depend on the owner
  // classifier, so normal constructor injection keeps the TU-anchor/TU-span
  // boundary explicit without creating a service cycle.
  ownerClassifier_ =
      std::make_unique<RefoldOwnerClassifier>(RefoldOwnerClassifier::Deps{
          model_, pathIdentity_, TUEditPlanner(), sidebandPragmaEdits_});
}

RefoldOwnerClassifier &RefoldEngine::OwnerClassifier() {
  assert(ownerClassifier_ && "owner classifier service not initialized");
  return *ownerClassifier_;
}

const RefoldOwnerClassifier &RefoldEngine::OwnerClassifier() const {
  assert(ownerClassifier_ && "owner classifier service not initialized");
  return *ownerClassifier_;
}

void RefoldEngine::InitializeBInsertionLedger() {
  // Pure B-token insertion ownership is a named edit-domain ledger.  The
  // ledger borrows the owner classifier and macro-boundary selector directly;
  // standalone preclaims depend only on these explicit services and the
  // ledger's own run-local state.
  bInsertionLedger_ = std::make_unique<RefoldBInsertionLedger>(
      RefoldBInsertionLedger::Deps{bToks_, sourceMapper_, OwnerClassifier(),
                                   macroTopology_, macroBoundarySelector_});
}

RefoldBInsertionLedger &RefoldEngine::BInsertionLedger() {
  assert(bInsertionLedger_ && "B insertion ledger service not initialized");
  return *bInsertionLedger_;
}

const RefoldBInsertionLedger &RefoldEngine::BInsertionLedger() const {
  assert(bInsertionLedger_ && "B insertion ledger service not initialized");
  return *bInsertionLedger_;
}

void RefoldEngine::InitializePreprocessingStructureIndexProvider() {
  assert(preprocessingStructureIndex_ &&
         "TU structure index must precede the shared provider");
  preprocessingStructureIndexProvider_ =
      std::make_unique<RefoldPreprocessingStructureIndexProvider>(
          RefoldPreprocessingStructureIndexProvider::Dependencies{
              model_, pathIdentity_, lineDirs_, MacroStateProof(), lexLang_},
          model_.GetSourcePath(), *preprocessingStructureIndex_);
}

void RefoldEngine::InitializeTUAnchorProof() {
  // TU-anchor accepted-result construction is a narrow proof service.  Audit
  // flows through the shared theorem/audit service and summary construction
  // through the shared proof-summary builder, while the anchor proof builder
  // owns only TU-anchor carrier construction.
  tuAnchorProof_ =
      std::make_unique<RefoldTUAnchorProof>(TheoremAudit(), bToks_);
}

RefoldTUAnchorProof &RefoldEngine::TUAnchorProof() {
  assert(tuAnchorProof_ && "TU anchor proof service not initialized");
  return *tuAnchorProof_;
}

const RefoldTUAnchorProof &RefoldEngine::TUAnchorProof() const {
  assert(tuAnchorProof_ && "TU anchor proof service not initialized");
  return *tuAnchorProof_;
}

void RefoldEngine::InitializeTokenDiffPlanner() {
  assert(preprocessingStructureIndexProvider_ &&
         "structure-index provider must precede token diff planning");
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
}

void RefoldEngine::InitializeMixedOwnerTilingPlanner() {
  assert(preprocessingStructureIndexProvider_ &&
         "structure-index provider must precede structural tiling");
  assert(macroStateProof_ &&
         "macro-state proof must precede structural tiling");

  // Structural tiling borrows the owner/proof services and exact preprocessing
  // census needed to prove deterministic token-hunk partitions.  The planner
  // retains its historical type name while atomically publishing the normalized
  // token-hunk cache and durable witness ledgers.  PlanTokenDiff() validates
  // that publication before insertion provenance or owner dispatch can observe
  // hunk indices.
  mixedOwnerTilingPlanner_ = std::make_unique<RefoldMixedOwnerTilingPlanner>(
      RefoldMixedOwnerTilingPlanner::Dependencies{
          model_, model_.GetSourcePath(), pathIdentity_, macroTopology_,
          MacroStateProof(), tokenTextAnalysis_, sourceMapper_, tuSourceBytes_,
          bSource_, lexLang_,
          OwnerClassifier(), OwnerStateProof(),
          *preprocessingStructureIndexProvider_, abTokHunks_,
          mixedOwnerTilingWitnesses_, mixedOwnerTilingSegmentBindings_});
}

void RefoldEngine::InitializeTUEditPlanner() {
  assert(preprocessingStructureIndex_ &&
         "preprocessing-structure index must precede TU edit planning");

  // TU edit planning has a real owner.  The planner receives only read-only
  // services and token-map state; final TextEdit assembly intentionally remains
  // outside this service.
  tuEditPlanner_ =
      std::make_unique<RefoldTUEditPlanner>(RefoldTUEditPlanner::Deps{
          model_, pathIdentity_, macroTopology_, TUAnchorProof(), lineDirs_,
          *preprocessingStructureIndex_, tuSourceBytes_, aToks_,
          static_cast<uint64_t>(bToks_.size()), bToks_, abTokMapA2B_,
          ownerDepthGap_, strict_});
}

void RefoldEngine::InitializePreprocessingStructureIndex() {
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
                  model_, pathIdentity_, MacroStateProof(), lexLang_},
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
    REFOLD_LOG_DEBUG("tu/structure-index", "index diagnostic: {0}",
                     diagnostic);
  }
}

RefoldTUEditPlanner &RefoldEngine::TUEditPlanner() {
  assert(tuEditPlanner_ && "TU edit planner service not initialized");
  return *tuEditPlanner_;
}

const RefoldTUEditPlanner &RefoldEngine::TUEditPlanner() const {
  assert(tuEditPlanner_ && "TU edit planner service not initialized");
  return *tuEditPlanner_;
}

void RefoldEngine::InitializeCounterStabilization() {
  counterStabilization_ = std::make_unique<RefoldCounterStabilization>(
      model_, aToks_, bToks_, macroTopology_, OwnerClassifier());
}

RefoldCounterStabilization &RefoldEngine::CounterStabilization() {
  assert(counterStabilization_ &&
         "counter-stabilization service not initialized");
  return *counterStabilization_;
}

const RefoldCounterStabilization &RefoldEngine::CounterStabilization() const {
  assert(counterStabilization_ &&
         "counter-stabilization service not initialized");
  return *counterStabilization_;
}

void RefoldEngine::InitializeMacroStateProof() {
  macroStateProof_ = std::make_unique<RefoldMacroStateProof>(
      model_, pathIdentity_, tokenTextAnalysis_, OwnerStateProof());
}

void RefoldEngine::InitializeIncludeInsertionPlanner() {
  includeInsertionPlanner_ = std::make_unique<RefoldIncludeInsertionPlanner>(
      bSource_, bToks_, bTokOff_, sourceMapper_, ProofLattice());
}

RefoldIncludeInsertionPlanner &RefoldEngine::IncludeInsertionPlanner() {
  assert(includeInsertionPlanner_ &&
         "include-insertion planner service not initialized");
  return *includeInsertionPlanner_;
}

const RefoldIncludeInsertionPlanner &
RefoldEngine::IncludeInsertionPlanner() const {
  assert(includeInsertionPlanner_ &&
         "include-insertion planner service not initialized");
  return *includeInsertionPlanner_;
}

void RefoldEngine::InitializeLineObserverLayout() {
  lineObserverLayout_ = std::make_unique<RefoldLineObserverLayout>(
      model_, bSource_, aToks_, bToks_, bTokOff_, abTokMapA2B_, abTokMapB2A_,
      pathIdentity_, lineControlProof_, OwnerStateProof(), ProofLattice(),
      *textEditAssembler_, lineDirs_);
}

RefoldLineObserverLayout &RefoldEngine::LineObserverLayout() {
  assert(lineObserverLayout_ && "line-observer layout service not initialized");
  return *lineObserverLayout_;
}

const RefoldLineObserverLayout &RefoldEngine::LineObserverLayout() const {
  assert(lineObserverLayout_ && "line-observer layout service not initialized");
  return *lineObserverLayout_;
}

void RefoldEngine::InitializeTheoremAudit() {
  // The audit is built before the lattice because the lattice takes the audit
  // by reference.  The reverse edge -- the audit's five lattice queries -- is
  // bound by BindProofLattice() once InitializeProofLattice() has run.
  theoremAudit_ = std::make_unique<RefoldTheoremAudit>(
      lastTheoremAudit_, terminalSink_, strict_,
      alignmentSemanticTheoremActive_);
}

RefoldTheoremAudit &RefoldEngine::TheoremAudit() const {
  assert(theoremAudit_ && "theorem/audit service not initialized");
  return *theoremAudit_;
}

void RefoldEngine::InitializeOwnerStateProof() {
  RefoldOwnerStateProofInputs inputs{model_, aToks_, bToks_, lexLang_,
                                     ownerStateGraphMemo_};
  ownerStateProof_ = std::make_unique<RefoldOwnerStateProof>(
      inputs, pathIdentity_, tokenTextAnalysis_, macroTopology_, TheoremAudit(),
      terminalSink_);
}

void RefoldEngine::AdoptOwnerStateGraphMemo(OwnerStateGraphMemo *memo) {
  ownerStateGraphMemo_ = memo;
  if (ownerStateProof_)
    ownerStateProof_->SetOwnerStateGraphMemo(memo);
}

RefoldOwnerStateProof &RefoldEngine::OwnerStateProof() {
  return *ownerStateProof_;
}

const RefoldOwnerStateProof &RefoldEngine::OwnerStateProof() const {
  return *ownerStateProof_;
}

void RefoldEngine::InitializeProofLattice() {
  // The proof lattice receives only the inputs and callbacks it needs.  TU
  // owner/anchor diagnostics use RefoldTUEditPlanner through constructor
  // injection, which keeps TU planning out of the callback bundle and makes the
  // late-bound planner/lattice dependency explicit.
  RefoldProofLattice::Hooks hooks;
  // Non-audit orchestration hook.
  hooks.computeWholeCoverPlan =
      [this](const RefoldModel::MacroInvocation &macro) {
        return MacroPatchPlanner().ComputeWholeCoverPlan(macro);
      };

  proofLattice_ = std::make_unique<RefoldProofLattice>(
      model_, bSource_, bToks_, sourceMapper_, tokenTextAnalysis_,
      argTextRecovery_, macroTopology_, OwnerStateProof(), terminalSink_,
      TUEditPlanner(), TheoremAudit(), lastTheoremAudit_, strict_,
      proofAuditMode_, alignmentSemanticTheoremActive_,
      mixedOwnerTilingSegmentBindings_,
      mixedOwnerTilingWitnesses_, std::move(hooks));

  // Close the audit/lattice construction cycle now that both services exist.
  TheoremAudit().BindProofLattice(*proofLattice_);
}

RefoldProofLattice &RefoldEngine::ProofLattice() { return *proofLattice_; }

const RefoldProofLattice &RefoldEngine::ProofLattice() const {
  return *proofLattice_;
}

void RefoldEngine::InitializeMacroPatchPlanner() {
  // The planner is constructed after the proof services it borrows.  The
  // dependency bundle is intentionally explicit: macro planning reads source
  // data and proof services directly rather than calling back into
  // RefoldEngine.
  RefoldMacroPatchPlanner::Dependencies deps;
  deps.model = &model_;
  deps.bSource = bSource_;
  deps.aToks = aToks_;
  deps.bToks = bToks_;
  deps.bTokOff = bTokOff_;
  deps.abTokHunks = &abTokHunks_;
  deps.bInsertionLedger = &BInsertionLedger();
  deps.argTextRecovery = &argTextRecovery_;
  deps.lexLang = &lexLang_;
  deps.lineDirs = &lineDirs_;
  deps.macroTopology = &macroTopology_;
  deps.pathIdentity = &pathIdentity_;
  deps.sourceMapper = &sourceMapper_;
  deps.ownerClassifier = &OwnerClassifier();
  deps.strict = strict_;
  deps.ownersMustExpand = &ownersMustExpand_;

  deps.macroStateProof = &MacroStateProof();
  deps.ownerStateProof = &OwnerStateProof();
  deps.proofLattice = &ProofLattice();
  macroPatchPlanner_ =
      std::make_unique<RefoldMacroPatchPlanner>(std::move(deps));
}

RefoldMacroPatchPlanner &RefoldEngine::MacroPatchPlanner() {
  assert(macroPatchPlanner_ && "macro-patch planner service not initialized");
  return *macroPatchPlanner_;
}

const RefoldMacroPatchPlanner &RefoldEngine::MacroPatchPlanner() const {
  assert(macroPatchPlanner_ && "macro-patch planner service not initialized");
  return *macroPatchPlanner_;
}

void RefoldEngine::InitializeMacroStateRepairPlanner() {
  RefoldMacroStateRepairPlanner::Dependencies deps;
  deps.model = &model_;
  deps.pathIdentity = &pathIdentity_;
  deps.macroTopology = &macroTopology_;
  deps.tokenTextAnalysis = &tokenTextAnalysis_;
  deps.macroStateProof = &MacroStateProof();
  deps.ownerStateProof = &OwnerStateProof();
  deps.proofLattice = &ProofLattice();
  deps.macroPatchPlanner = &MacroPatchPlanner();
  deps.textEditAssembler = textEditAssembler_.get();
  deps.terminalSink = &terminalSink_;
  deps.lexLang = &lexLang_;
  macroStateRepairPlanner_ =
      std::make_unique<RefoldMacroStateRepairPlanner>(std::move(deps));
}

RefoldMacroStateRepairPlanner &RefoldEngine::MacroStateRepairPlanner() {
  assert(macroStateRepairPlanner_ &&
         "macro-state repair planner service not initialized");
  return *macroStateRepairPlanner_;
}

const RefoldMacroStateRepairPlanner &
RefoldEngine::MacroStateRepairPlanner() const {
  assert(macroStateRepairPlanner_ &&
         "macro-state repair planner service not initialized");
  return *macroStateRepairPlanner_;
}

//===----------------------------------------------------------------------===//
// Text-edit assembler construction
//===----------------------------------------------------------------------===//
//
// The assembler owns final byte-application behavior.  Theorem/audit policy is
// injected as RefoldTheoremAudit; the remaining hook bundle is limited to
// non-audit orchestration that still belongs to the engine service graph.

void RefoldEngine::InitializeTextEditAssembler() {
  RefoldTextEditAssembler::Hooks hooks;
  // Non-audit orchestration hooks.
  hooks.computeWholeCoverPlan =
      [this](const RefoldModel::MacroInvocation &macro)
      -> std::optional<WholeCoverPlan> {
    return MacroPatchPlanner().ComputeWholeCoverPlan(macro);
  };
  hooks.lineResyncShouldDeferToConditionalJoin =
      [this](StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
             uint64_t resumeOffset) {
        return LineObserverLayout().LineResyncShouldDeferToConditionalJoin(
            ownerFile, ownerIncludeId, resumeOffset);
      };

  textEditAssembler_ = std::make_unique<RefoldTextEditAssembler>(
      model_, bSource_, aToks_, bToks_, bTokOff_, abTokHunks_, abTokMapA2B_,
      abTokMapB2A_, sourceMapper_, pathIdentity_, MacroStateProof(), lexLang_,
      *preprocessingStructureIndex_, ProofLattice(), OwnerStateProof(),
      macroTopology_, lineControlProof_, lineDirs_, terminalSink_,
      TUEditPlanner(), TheoremAudit(), sidebandPragmaEdits_,
      mixedOwnerTilingWitnesses_, lastTheoremAudit_, std::move(hooks));
}

//===----------------------------------------------------------------------===//
// Include-materializer construction
//===----------------------------------------------------------------------===//
//
// The materializer receives explicit proof/text/edit services.  Include proof,
// topology, and terminal services are named constructor dependencies, while
// lexical boundary padding remains a shared helper rather than another
// one-method service.

void RefoldEngine::InitializeIncludeMaterializer() {
  includeMaterializer_ = std::make_unique<RefoldIncludeMaterializer>(
      model_, aSource_, bSource_, aToks_, bToks_, bTokOff_, abTokMapA2B_,
      lineDirs_, finalReplaySurface_, sidebandPragmaEdits_, sourceMapper_,
      pathIdentity_, macroTopology_, lineControlProof_, LineObserverLayout(),
      MacroStateProof(), OwnerStateProof(), IncludeInsertionPlanner(),
      ProofLattice(), *textEditAssembler_, PragmaOnceGuardRewriter(),
      terminalSink_, lexLang_);
}

//===----------------------------------------------------------------------===//
// Pragma-once guard rewriter construction
//===----------------------------------------------------------------------===//
//
// The catalog is built from producer include/pragma facts and the physical
// header bytes, so it must follow the text-edit assembler and proof lattice it
// authorizes edits through.  The set of headers actually inlined is recorded
// later, by include-materialization scheduling.

void RefoldEngine::InitializePragmaOnceGuardRewriter() {
  pragmaOnceGuardRewriter_ = std::make_unique<RefoldPragmaOnceGuardRewriter>(
      RefoldPragmaOnceGuardRewriter::Dependencies{
          model_, pathIdentity_, MacroStateProof(), lineDirs_,
          lineControlProof_, *textEditAssembler_, ProofLattice(),
          terminalSink_, lexLang_},
      RefoldPragmaOnceGuardRewriter::GuardNameInputs{
          model_.GetSourcePath(), tuSourceBytes_, aSource_, bSource_});
}

RefoldPragmaOnceGuardRewriter &RefoldEngine::PragmaOnceGuardRewriter() {
  assert(pragmaOnceGuardRewriter_ &&
         "pragma-once guard rewriter must be initialized");
  return *pragmaOnceGuardRewriter_;
}

const RefoldPragmaOnceGuardRewriter &
RefoldEngine::PragmaOnceGuardRewriter() const {
  assert(pragmaOnceGuardRewriter_ &&
         "pragma-once guard rewriter must be initialized");
  return *pragmaOnceGuardRewriter_;
}

//===----------------------------------------------------------------------===//
// Expansion-fallback planner construction
//===----------------------------------------------------------------------===//
//
// The fallback planner is allocated after the materialization and proof
// services it calls into are available.  Its hook bundle is intentionally
// visible here as service-graph wiring for non-audit emission/orchestration
// decisions.

void RefoldEngine::InitializeExpansionFallbackPlanner() {
  assert(preprocessingStructureIndex_ &&
         "preprocessing-structure index must precede expansion fallback");
  RefoldExpansionFallbackPlanner::Hooks hooks;
  // Non-audit emission/orchestration hooks.  These callbacks route directly to
  // the assembler service; the hook bundle stays limited to the cycle-breaking
  // boundary required by fallback emission.
  hooks.applyResyncOrPend = [this](StringRef originalFileText, uint64_t start,
                                   uint64_t end, StringRef replacement,
                                   StringRef fileSpellingForDirective,
                                   std::optional<uint64_t> ownerIncludeId) {
    return textEditAssembler_->ApplyResyncOrPend(
        originalFileText, start, end, replacement, fileSpellingForDirective,
        ownerIncludeId);
  };
  hooks.certifyTextEditMaterializedBTokenRange =
      [this](TextEdit &edit, uint64_t bTokBegin, uint64_t bTokEnd) {
        textEditAssembler_->CertifyTextEditMaterializedBTokenRange(
            edit, bTokBegin, bTokEnd);
      };
  hooks.attachAcceptedResultCarrier =
      [this](TextEdit &edit, const AcceptedResultCandidate &candidate) {
        textEditAssembler_->AttachAcceptedResultCarrier(edit, candidate);
      };
  hooks.authorizeTUIncludeClosure =
      [this](TextEdit &edit, StringRef sourcePath, StringRef sourceBytes,
             uint64_t begin, uint64_t end) {
        return textEditAssembler_->AuthorizeCompleteProtectedSourceClosure(
            edit, ProtectedSourceEditAuthorityKind::TUIncludeClosure,
            sourcePath, std::nullopt, sourceBytes, begin, end,
            /*requireProtectedInterval=*/true,
            /*requestTerminalOnFailure=*/false);
      };
  hooks.resetAttemptStats = [this]() {
    resetRefoldAttemptStats(lastStats_, model_);
  };

  expansionFallbackPlanner_ = std::make_unique<RefoldExpansionFallbackPlanner>(
      model_, bSource_, aToks_, abTokHunks_, abTokMapB2A_, abTokAnchorProofs_,
      lineDirs_, sourceMapper_, pathIdentity_, macroTopology_,
      lineControlProof_, MacroStateProof(), *preprocessingStructureIndex_,
      terminalSink_, lexLang_, IncludeInsertionPlanner(), ProofLattice(),
      TheoremAudit(), lastStats_, materializedEditMappings_, std::move(hooks));
}

} // namespace refold
} // namespace clang

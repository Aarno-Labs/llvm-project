//===--- RefoldIncludeMaterializer.h ----------------------------*- C++ -*-===//
//
// Include materialization and include-owned edit planning for clang-refold.
//
// RefoldIncludeMaterializer owns the deterministic realization path for include
// subtrees: inline realization from B, header-local include edits, child-include
// boundary anchoring, macro-state carry decisions, and final materialized
// include text assembly.  All immutable run inputs, proof services, text-
// assembly services, and orchestration callbacks are supplied explicitly at
// construction time.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEMATERIALIZER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEMATERIALIZER_H

#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldProofTypes.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTerminalProofSink.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"
#include "util/RefoldDenseMapInfo.h"
#include "util/RefoldPathIdentity.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldOwnerStateProof;
class RefoldProofLattice;
class RefoldLineObserverLayout;
class RefoldSourceMapper;
class RefoldTextEditAssembler;

/// Builds include-owned insertion patches and include-realization
/// B-token envelopes.  This helper is co-located with the include
/// materializer because it is only useful to include materialization and
/// fallback paths; keeping it here avoids a tiny standalone translation unit
/// without changing the deterministic staging/proof policy.
class RefoldIncludeInsertionPlanner {
public:
  RefoldIncludeInsertionPlanner(llvm::StringRef bSource,
                                llvm::ArrayRef<PPTok> bToks,
                                llvm::ArrayRef<size_t> bTokOff,
                                const RefoldSourceMapper &sourceMapper,
                                const RefoldProofLattice &proofLattice);

  /// Resolve an include-realization B-token envelope from an A-token cover.
  ///
  /// The canonical path is the ordinary A-cover -> B-envelope mapper.  If that
  /// proof is unavailable, this method permits exactly the declared
  /// BoundaryStableConsensusBCoverEnvelope proof: all usable non-canonical
  /// boundary-stable projections must agree on the same non-empty B-token
  /// envelope.  Missing or empty projections are ignored, but conflicting usable
  /// projections fail closed.
  std::optional<std::pair<size_t, size_t>>
  ResolveIncludeRealizationBTokenEnvelope(
      uint64_t beginTok, uint64_t endTok,
      IncludeRealizationEvidenceKind *evidenceKind = nullptr) const;

  /// Build an include-owned staging patch from an already-attributed diff hunk.
  ///
  /// The returned IncludePatch carries the A/B token intervals from \p h and the
  /// exact bytes copied from B.  Replacement/deletion hunks use exact token
  /// coverage so surrounding inter-token whitespace stays with the neighboring
  /// owner; pure insertions keep the full token envelope because there is no
  /// original source span whose boundary whitespace can be retained.
  IncludePatch BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                          const diffutils::Hunk &h) const;

private:
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> bTokOff_;
  const RefoldSourceMapper &sourceMapper_;
  const RefoldProofLattice &proofLattice_;
};

class RefoldIncludeMaterializer {
public:
  using AcceptedPathKind = ::clang::refold::AcceptedPathKind;
  using AcceptedResultCandidate = ::clang::refold::AcceptedResultCandidate;
  using IncludeAnchorEvidenceKind = ::clang::refold::IncludeAnchorEvidenceKind;
  using IncludeAnchorWitness = ::clang::refold::IncludeAnchorWitness;
  using IncludeEdits = ::clang::refold::IncludeEdits;
  using IncludePatch = ::clang::refold::IncludePatch;
  using IncludeRealizationEvidenceKind =
      ::clang::refold::IncludeRealizationEvidenceKind;
  using IncludeTextEditPlan = ::clang::refold::IncludeTextEditPlan;
  using LineControlWrappedText = ::clang::refold::LineControlWrappedText;
  using MacroPatch = ::clang::refold::MacroPatch;
  using MacroStateDirectiveLineInterval =
      ::clang::refold::MacroStateDirectiveLineInterval;
  using OwnerSourceRange = ::clang::refold::OwnerSourceRange;
  using OwnerStateBoundary = ::clang::refold::OwnerStateBoundary;
  using OwnerStateComponent = ::clang::refold::OwnerStateComponent;
  using ResyncOutcome = ::clang::refold::ResyncOutcome;
  using SelectedAcceptedResultCandidate =
      ::clang::refold::SelectedAcceptedResultCandidate;
  using StabilizedMaterializedHeaderMacroPatch =
      ::clang::refold::StabilizedMaterializedHeaderMacroPatch;
  using StateMutationKind = ::clang::refold::StateMutationKind;
  using SuffixStabilityWitnessKind = ::clang::refold::SuffixStabilityWitnessKind;
  using TextEdit = ::clang::refold::TextEdit;

  RefoldIncludeMaterializer(
      const RefoldModel &model, llvm::StringRef aSource,
      llvm::StringRef bSource, llvm::ArrayRef<PPTok> aToks,
      llvm::ArrayRef<PPTok> bToks, llvm::ArrayRef<size_t> bTokOff,
      const std::vector<int64_t> &abTokMapA2B,
      const LineDirectiveInserter &lineDirs,
      const std::optional<FinalReplaySurface> &finalReplaySurface,
      const std::vector<SidebandPragmaEdit> &sidebandPragmaEdits,
      const RefoldSourceMapper &sourceMapper,
      const RefoldPathIdentity &paths,
      const RefoldMacroTopology &macroTopology,
      const RefoldLineControlProof &lineControlProof,
      const RefoldLineObserverLayout &lineObserverLayout,
      const RefoldMacroStateProof &macroStateProof,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldIncludeInsertionPlanner &includeInsertionPlanner,
      const RefoldProofLattice &proofLattice,
      const RefoldTextEditAssembler &textEditAssembler,
      const RefoldTerminalProofSink &terminalSink,
      const clang::LangOptions &lexLang)
      : model_(model), aSource_(aSource), bSource_(bSource),
        aToks_(aToks), bToks_(bToks), bTokOff_(bTokOff),
        abTokMapA2B_(abTokMapA2B), lineDirs_(lineDirs),
        finalReplaySurface_(finalReplaySurface),
        sidebandPragmaEdits_(sidebandPragmaEdits), sourceMapper_(sourceMapper),
        paths_(paths), macroTopology_(macroTopology),
        lineControlProof_(lineControlProof),
        lineObserverLayout_(lineObserverLayout),
        macroStateProof_(macroStateProof),
        ownerStateProof_(ownerStateProof),
        includeInsertionPlanner_(includeInsertionPlanner),
        proofLattice_(proofLattice), textEditAssembler_(textEditAssembler),
        terminalSink_(terminalSink), lexLang_(lexLang) {}

  /// Realize an include expansion directly from the edited preprocessed stream
  /// B after the include-realization envelope has been proven by the shared
  /// accepted-result machinery.
  std::optional<std::string> BuildInlineIncludeRealizationFromB(
      const RefoldModel::IncludeItem &inc, llvm::StringRef reason,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// Fully materialize one include instance, recursively realizing any child
  /// include that cannot remain a proven source-spelled include directive.
  void MaterializeIncludeExpansion(
      uint64_t includeId, const llvm::DenseMap<uint64_t, IncludeEdits> &perInclude,
      const llvm::DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
          &macroPatchesByOwner,
      const llvm::DenseMap<uint64_t,
                           std::vector<const RefoldModel::IncludeItem *>>
          &children,
      llvm::DenseMap<uint64_t, std::string> &includeExpansion,
      llvm::DenseMap<uint64_t, std::vector<FinalLineControlPruneCandidate>>
          &includeExpansionLineControlPruneCandidates,
      llvm::DenseMap<uint64_t, std::vector<FinalLineControlSourceMapping>>
          &includeExpansionLineControlSourceMappings,
      llvm::DenseMap<uint64_t, size_t> &includeExpansionStartLineNos,
      llvm::DenseMap<uint64_t, AcceptedResultCandidate>
          &includeExpansionAcceptedResults,
      llvm::DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr,
      bool materializeIncludeNextInThisSubtree = false) const;

  /// Select the recorded header declaration that best owns an include-scoped
  /// patch.  This remains static because it is a pure declaration-span chooser
  /// and does not need engine state.
  static const RefoldModel::HeaderDecl *
  FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                         const IncludePatch &p);

  /// Compute header-local byte edits for include-scoped patches without
  /// applying them.  Returned edits are sorted for high-to-low application.
  IncludeTextEditPlan ComputeIncludeTextEdits(const IncludeEdits &ie,
                                              std::string headerText) const;

  /// Prove a parent-header pure insertion anchor at a preserved direct child
  /// include directive boundary.
  std::optional<uint64_t> ComputeChildBoundaryInsertByte(
      const IncludePatch &p, llvm::StringRef file,
      IncludeAnchorWitness *witness = nullptr) const;

private:
  /// Return the exact producer file byte start for an A-side PP coordinate.
  /// This helper remains exact-only and uses RefoldPathIdentity for physical
  /// spelling comparison rather than manufacturing EOF anchors.
  std::optional<uint64_t> ByteStartForPPInFile(llvm::StringRef file,
                                               uint64_t pp) const;

  /// Return the exact producer file byte end for an A-side PP coordinate.
  std::optional<uint64_t> ByteEndForPPInFile(llvm::StringRef file,
                                             uint64_t pp) const;

  /// Build a TextEdit while applying local or pending #line resync behavior
  /// through the text-edit assembler service.
  TextEdit MakeTextEditWithResyncOrPending(
      llvm::StringRef original, uint64_t start, uint64_t end,
      llvm::StringRef replacement, llvm::StringRef fileSpelling,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// Validate an include-owned sideband pragma edit and record the same
  /// terminal-fallback obligation that the engine wrapper records for TU-owned
  /// sideband edits.
  bool ValidateSidebandPragmaEditProof(const SidebandPragmaEdit &edit,
                                       llvm::StringRef stage) const;

  const RefoldModel &model_;
  llvm::StringRef aSource_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> bTokOff_;
  const std::vector<int64_t> &abTokMapA2B_;
  const LineDirectiveInserter &lineDirs_;
  const std::optional<FinalReplaySurface> &finalReplaySurface_;
  const std::vector<SidebandPragmaEdit> &sidebandPragmaEdits_;
  const RefoldSourceMapper &sourceMapper_;
  const RefoldPathIdentity &paths_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldLineControlProof &lineControlProof_;
  const RefoldLineObserverLayout &lineObserverLayout_;
  const RefoldMacroStateProof &macroStateProof_;
  const RefoldOwnerStateProof &ownerStateProof_;
  const RefoldIncludeInsertionPlanner &includeInsertionPlanner_;
  const RefoldProofLattice &proofLattice_;
  const RefoldTextEditAssembler &textEditAssembler_;
  const RefoldTerminalProofSink &terminalSink_;
  const clang::LangOptions &lexLang_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEMATERIALIZER_H

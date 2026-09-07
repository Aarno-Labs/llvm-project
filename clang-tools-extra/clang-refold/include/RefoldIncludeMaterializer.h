//===--- RefoldIncludeMaterializer.h ----------------------------*- C++ -*-===//
//
// Include materialization and include-owned edit planning for clang-refold.
//
// RefoldIncludeMaterializer owns the deterministic realization path for include
// subtrees: inline realization from B, header-local include edits,
// child-include boundary anchoring, macro-state carry decisions, and final
// materialized include text assembly.  All immutable run inputs, proof
// services, text-assembly services, and orchestration callbacks are supplied
// explicitly at construction time.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEMATERIALIZER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEMATERIALIZER_H

#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "include/RefoldIncludeReplayProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAnchorWitnessTypes.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTerminalProofSink.h"
#include "proof/RefoldTheoremTypes.h"
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
class RefoldPragmaOnceGuardRewriter;
class RefoldSourceMapper;
class RefoldTextEditAssembler;

class RefoldIncludeInsertionPlanner;

/// Materializes include-owned edits after include replay proof and scheduling.
///
/// The service builds inline include realization text, coordinates recursive
/// materialization, attaches source-graph/line-control evidence, and emits
/// include-local patch buckets without owning include search-chain proof.
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
  using ResyncOutcome = ::clang::refold::ResyncOutcome;
  using SelectedAcceptedResultCandidate =
      ::clang::refold::SelectedAcceptedResultCandidate;
  using StabilizedMaterializedHeaderMacroPatch =
      ::clang::refold::StabilizedMaterializedHeaderMacroPatch;
  using TextEdit = ::clang::refold::TextEdit;

  /// Construct an include materializer over immutable A/B token/source inputs
  /// and explicit proof/edit services.
  ///
  /// The materializer does not own pass-level staging maps.  Callers pass those
  /// maps to the realization methods so recursive include materialization stays
  /// deterministic and run-scoped.
  RefoldIncludeMaterializer(
      const RefoldModel &model, llvm::StringRef aSource,
      llvm::StringRef bSource, llvm::ArrayRef<PPTok> aToks,
      llvm::ArrayRef<PPTok> bToks, llvm::ArrayRef<size_t> bTokOff,
      const std::vector<int64_t> &abTokMapA2B,
      const LineDirectiveInserter &lineDirs,
      const std::optional<FinalReplaySurface> &finalReplaySurface,
      const std::vector<SidebandPragmaEdit> &sidebandPragmaEdits,
      const RefoldSourceMapper &sourceMapper, const RefoldPathIdentity &paths,
      const RefoldMacroTopology &macroTopology,
      const RefoldLineControlProof &lineControlProof,
      const RefoldLineObserverLayout &lineObserverLayout,
      const RefoldMacroStateProof &macroStateProof,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldIncludeInsertionPlanner &includeInsertionPlanner,
      const RefoldProofLattice &proofLattice,
      const RefoldTextEditAssembler &textEditAssembler,
      const RefoldPragmaOnceGuardRewriter &pragmaOnceGuards,
      const RefoldTerminalProofSink &terminalSink,
      const clang::LangOptions &lexLang)
      : model_(model), aSource_(aSource), bSource_(bSource), aToks_(aToks),
        bToks_(bToks), bTokOff_(bTokOff), abTokMapA2B_(abTokMapA2B),
        lineDirs_(lineDirs), finalReplaySurface_(finalReplaySurface),
        sidebandPragmaEdits_(sidebandPragmaEdits), sourceMapper_(sourceMapper),
        paths_(paths), macroTopology_(macroTopology),
        lineControlProof_(lineControlProof),
        lineObserverLayout_(lineObserverLayout),
        macroStateProof_(macroStateProof), ownerStateProof_(ownerStateProof),
        includeInsertionPlanner_(includeInsertionPlanner),
        proofLattice_(proofLattice), textEditAssembler_(textEditAssembler),
        pragmaOnceGuards_(pragmaOnceGuards), terminalSink_(terminalSink),
        lexLang_(lexLang) {}

  /// Realize an include expansion directly from the edited preprocessed stream
  /// B after the include-realization envelope has been proven by the shared
  /// accepted-result machinery.
  ///
  /// Inline include realization is theorem-facing only when the include's
  /// A-cover can be projected to a concrete B-token envelope by an accepted
  /// witness: either the canonical A-cover mapping or the boundary-stable
  /// consensus proof.  If no such envelope exists, callers must use the
  /// explicit terminal-fallback path instead of synthesizing a weaker
  /// realization.
  std::optional<std::string> BuildInlineIncludeRealizationFromB(
      const RefoldModel::IncludeItem &inc, llvm::StringRef reason,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// Fully materialize one include instance, recursively realizing any child
  /// include that cannot remain a proven source-spelled include directive.
  ///
  /// Parent includes are materialized before descendants, but child directives
  /// are not blindly expanded: include replay proof decides whether each clean
  /// child directive can be preserved, rewritten, or must be materialized.
  /// Macro patches, include-local patches, line-control pruning/mapping
  /// records, and accepted-result carriers are all accumulated through
  /// caller-owned run-scoped maps.
  void MaterializeIncludeExpansion(
      uint64_t includeId,
      const llvm::DenseMap<uint64_t, IncludeEdits> &perInclude,
      const llvm::DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
          &macroPatchesByOwner,
      const llvm::DenseMap<
          uint64_t, std::vector<const RefoldModel::IncludeItem *>> &children,
      llvm::DenseMap<uint64_t, std::string> &includeExpansion,
      llvm::DenseMap<uint64_t, std::vector<FinalLineControlPruneCandidate>>
          &includeExpansionLineControlPruneCandidates,
      llvm::DenseMap<uint64_t, std::vector<FinalLineControlSourceMapping>>
          &includeExpansionLineControlSourceMappings,
      llvm::DenseMap<uint64_t, size_t> &includeExpansionStartLineNos,
      llvm::DenseMap<uint64_t, AcceptedResultCandidate>
          &includeExpansionAcceptedResults,
      llvm::DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr,
      bool materializeIncludeNextInThisSubtree = false,
      std::optional<uint64_t> ancestorArmIdAtIncludeSite = std::nullopt,
      const llvm::DenseSet<uint64_t> *ownersMustExpand = nullptr) const;

  /// Return the innermost conditional arm enclosing one include's own site,
  /// combined with the arm chain already accumulated from its ancestors.
  ///
  /// Once-state proofs need the arm chain across include boundaries, not only
  /// inside one file: a pragma that is unconditional within its header still does
  /// not dominate when the header itself was included from inside a conditional.
  /// The chain is threaded through materialization rather than recovered from
  /// `IncludeItem::parent`, because a skipped include edge carries no parent.
  /// \p ownerIncludeId is the include instance owning the site's file, or
  /// nullopt for a translation-unit-owned site.
  std::optional<uint64_t> ComputeAncestorArmForChildInclude(
      const RefoldModel::IncludeItem &child,
      std::optional<uint64_t> ownerIncludeId,
      std::optional<uint64_t> ancestorArmIdAtIncludeSite) const;

  /// Select the recorded header declaration that best owns an include-scoped
  /// patch.  This remains static because it is a pure declaration-span chooser
  /// and does not need engine state.
  static const RefoldModel::HeaderDecl *
  FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                         const IncludePatch &p);

  /// Compute header-local byte edits for include-scoped patches without
  /// applying them.
  ///
  /// Returned edits are sorted for high-to-low application so byte offsets stay
  /// valid while the caller splices the materialized header text.  The plan
  /// also carries line-control prune/source-mapping candidates that must remain
  /// tied to the include-local realization.
  IncludeTextEditPlan ComputeIncludeTextEdits(const IncludeEdits &ie,
                                              std::string headerText) const;

  /// Prove a parent-header pure insertion anchor at a preserved direct child
  /// include directive boundary.
  ///
  /// The returned byte offset is valid only when the include patch is a pure
  /// insertion whose owner-local boundary is exactly a direct child include
  /// site.  When requested, \p witness records the include-boundary evidence
  /// used by the accepted-result proof.
  std::optional<uint64_t>
  ComputeChildBoundaryInsertByte(const IncludePatch &p, llvm::StringRef file,
                                 IncludeAnchorWitness *witness = nullptr) const;

private:
  /// Collect the include instances whose content a body realized from B
  /// absorbed: this include and every descendant the producer actually entered.
  void CollectEnteredIncludeSubtree(
      uint64_t includeId, llvm::SmallVectorImpl<uint64_t> &subtree) const;

  /// Returns whether a path names the header currently being materialized.
  /// Logical and load-path spellings are both accepted, with physical-file
  /// equivalence used only as a hardening predicate.
  bool PathNamesMaterializedHeader(llvm::StringRef path,
                                   llvm::StringRef headerPath,
                                   llvm::StringRef headerLoadPath) const;

  /// Returns the producer-B realization reason for an unsafe copied header, if
  /// any.  The policy rejects source-spelled relocation when builtins or
  /// conditional control would observe a different replay context.
  std::optional<std::string> MaterializedHeaderRequiresBRealizationReason(
      const RefoldModel::IncludeItem &include, llvm::StringRef headerPath,
      llvm::StringRef headerLoadPath) const;

  /// Records an inline include realization from the producer-proven B envelope.
  /// The caller-owned realization maps are updated together so recursive
  /// include materialization observes a complete memoized result.
  bool TryRecordInlineIncludeRealizationFromB(
      const RefoldModel::IncludeItem &include, llvm::StringRef reason,
      llvm::DenseMap<uint64_t, std::string> &includeExpansion,
      llvm::DenseMap<uint64_t, size_t> &includeExpansionStartLineNos,
      llvm::DenseMap<uint64_t, AcceptedResultCandidate>
          &includeExpansionAcceptedResults,
      std::optional<uint64_t> ancestorArmIdAtIncludeSite = std::nullopt) const;

  /// Builds the parent-surface edit that rewrites a clean child include.
  /// The edit is accepted only when include replay proof has already supplied a
  /// deterministic ordinary include operand and delimiter.
  std::optional<TextEdit> MakeCleanChildIncludeOperandRewriteEdit(
      const RefoldModel::IncludeItem &child, llvm::StringRef rewrittenOperand,
      IncludeReplayProofContext::OrdinaryIncludeDelimiterKind delimiterKind,
      llvm::StringRef ownerBytes) const;

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
  const RefoldPragmaOnceGuardRewriter &pragmaOnceGuards_;
  const RefoldTerminalProofSink &terminalSink_;
  const clang::LangOptions &lexLang_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEMATERIALIZER_H

//===--- RefoldExpansionFallbackPlanner.h ----------------------*- C++ -*-===//
//
// Deterministic expansion-fallback planning for clang-refold.
//
// RefoldExpansionFallbackPlanner owns the two explicit expansion-fallback
// surfaces: TU include-closure recovery for an otherwise unresolved hunk, and
// final post-structural terminal fallback normalization.  All immutable inputs,
// proof services, mutable ledgers, and orchestration callbacks are supplied
// explicitly at construction time.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEXPANSIONFALLBACKPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEXPANSIONFALLBACKPLANNER_H

#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTopology.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include "clang/Basic/LangOptions.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldAcceptedCandidateBuilder;
class RefoldIncludeInsertionPlanner;
class RefoldLineObserverLayout;
class RefoldPreprocessingStructureIndex;
class RefoldTextEditCertifier;
class RefoldTheoremAudit;

/// Plans explicit expansion fallback when no declared structural proof owns a
/// hunk.  The planner builds fallback edits, records theorem-audit evidence,
/// and requests terminal fallback for strict-mode proof-domain escapes without
/// silently authorizing raw-B output.
class RefoldExpansionFallbackPlanner {
public:
  using TextEdit = ::clang::refold::TextEdit;
  using ResyncOutcome = ::clang::refold::ResyncOutcome;

  RefoldExpansionFallbackPlanner(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> aToks,
      const std::vector<diffutils::Hunk> &abTokHunks,
      const std::vector<int64_t> &abTokMapB2A,
      const std::vector<diffutils::LcsAnchorProof> &abTokAnchorProofs,
      const LineDirectiveInserter &lineDirs,
      const RefoldSourceMapper &sourceMapper, const RefoldPathIdentity &paths,
      const RefoldMacroTopology &macroTopology,
      const RefoldLineControlProof &lineControlProof,
      const RefoldMacroStateProof &macroStateProof,
      const RefoldPreprocessingStructureIndex &preprocessingStructureIndex,
      const RefoldTerminalProofSink &terminalSink,
      const clang::LangOptions &lexLang,
      const RefoldIncludeInsertionPlanner &includeInsertionPlanner,
      const RefoldAcceptedCandidateBuilder &acceptedCandidateBuilder,
      const RefoldTheoremAudit &theoremAuditService, RefoldStats &lastStats,
      std::vector<MaterializedEditMapping> *materializedEditMappings,
      const RefoldTextEditCertifier &textEditCertifier,
      const RefoldLineObserverLayout &lineObserverLayout)
      : model_(model), bSource_(bSource), aToks_(aToks),
        abTokHunks_(abTokHunks), abTokMapB2A_(abTokMapB2A),
        abTokAnchorProofs_(abTokAnchorProofs), lineDirs_(lineDirs),
        sourceMapper_(sourceMapper), paths_(paths),
        macroTopology_(macroTopology), lineControlProof_(lineControlProof),
        macroStateProof_(macroStateProof),
        preprocessingStructureIndex_(preprocessingStructureIndex),
        terminalSink_(terminalSink), lexLang_(lexLang),
        includeInsertionPlanner_(includeInsertionPlanner),
        acceptedCandidateBuilder_(acceptedCandidateBuilder),
        theoremAuditService_(theoremAuditService), lastStats_(lastStats),
        materializedEditMappings_(materializedEditMappings),
        textEditCertifier_(textEditCertifier),
        lineObserverLayout_(lineObserverLayout) {}

  /// Try to realize one unresolved PP hunk as the explicit
  /// TUIncludeClosureEdit proof class.  The implementation remains fail-closed:
  /// it only returns an edit after discharging the include-cover, source-gap,
  /// B-envelope, line-control, macro-state, and already-staged-edit
  /// obligations.
  std::optional<TextEdit> BuildTUIncludeClosureEditForUnresolvedHunk(
      const diffutils::Hunk &h, llvm::StringRef tuPath, llvm::StringRef tuBytes,
      llvm::ArrayRef<std::pair<uint64_t, uint64_t>> stagedSourceIntervals)
      const;

  /// Normalize a failed structural pass onto the sole terminal out-of-domain
  /// carrier.  This planner does not perform a second proof search; all
  /// in-domain macro/include/TU cases must already have been accepted by the
  /// structural lattice before this terminal path is selected.
  std::string ResolvePostStructuralFallback();

private:
  /// True when the proposed widened A range would cover any token diff hunk
  /// besides \p realizedHunk, the hunk currently being realized.
  bool RangeHasForeignTokenDiff(const diffutils::Hunk &realizedHunk,
                                uint64_t begin, uint64_t end) const;

  /// Does \p root's include subtree own a pragma whose effect outlives it?
  ///
  /// Consuming a touched include is justified by the closure realizing its
  /// expansion; that does not account for pragma state surviving the include's
  /// end, so the whole subtree is asked, not just the directly included file.
  bool
  IncludeSubtreeOwnsOutlivingPragma(const RefoldModel::IncludeItem &root) const;

  /// Return a concrete rejection reason when a TU gap contains a recorded
  /// pragma that blocks closure, or std::nullopt when none does.
  std::optional<std::string>
  NonConsumableTUPragmaGapReason(llvm::StringRef tuPath, uint64_t gapBegin,
                                 uint64_t gapEnd) const;

  const RefoldModel &model_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> aToks_;
  const std::vector<diffutils::Hunk> &abTokHunks_;
  const std::vector<int64_t> &abTokMapB2A_;
  /// Per-A-token authority for the selected alignment anchors.  A seam proof
  /// may rely only on `CoreOptimalPathForced` entries: a resolver-authorized
  /// anchor is admissible under a witness that assumed one normalized edit
  /// realization, so it cannot also authorize a different realization.
  const std::vector<diffutils::LcsAnchorProof> &abTokAnchorProofs_;
  const LineDirectiveInserter &lineDirs_;
  const RefoldSourceMapper &sourceMapper_;
  const RefoldPathIdentity &paths_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldLineControlProof &lineControlProof_;
  const RefoldMacroStateProof &macroStateProof_;
  /// Shared exact TU preprocessing census used by every source-gap theorem.
  const RefoldPreprocessingStructureIndex &preprocessingStructureIndex_;
  const RefoldTerminalProofSink &terminalSink_;
  const clang::LangOptions &lexLang_;
  const RefoldIncludeInsertionPlanner &includeInsertionPlanner_;
  const RefoldAcceptedCandidateBuilder &acceptedCandidateBuilder_;
  const RefoldTheoremAudit &theoremAuditService_;
  RefoldStats &lastStats_;
  std::vector<MaterializedEditMapping> *materializedEditMappings_;
  const RefoldTextEditCertifier &textEditCertifier_;
  const RefoldLineObserverLayout &lineObserverLayout_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEXPANSIONFALLBACKPLANNER_H

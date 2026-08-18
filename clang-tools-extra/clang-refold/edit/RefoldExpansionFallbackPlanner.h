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

#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include "clang/Basic/LangOptions.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldProofLattice;
class RefoldIncludeInsertionPlanner;
class RefoldPreprocessingStructureIndex;
class RefoldTheoremAudit;

/// Plans explicit expansion fallback when no declared structural proof owns a
/// hunk.  The planner builds fallback edits, records theorem-audit evidence,
/// and requests terminal fallback for strict-mode proof-domain escapes without
/// silently authorizing raw-B output.
class RefoldExpansionFallbackPlanner {
public:
  using TextEdit = ::clang::refold::TextEdit;
  using ResyncOutcome = ::clang::refold::ResyncOutcome;

  /// Exceptional callback bundle for engine-owned emission ledgers and attempt
  /// accounting.  Theorem/audit policy is a named service; these remaining
  /// hooks mutate final edit/resync state that is still owned
  /// by the engine/assembler boundary.
  struct Hooks {
    std::function<ResyncOutcome(llvm::StringRef, uint64_t, uint64_t,
                                llvm::StringRef, llvm::StringRef,
                                std::optional<uint64_t>)>
        applyResyncOrPend;
    std::function<void(TextEdit &, uint64_t, uint64_t)>
        certifyTextEditMaterializedBTokenRange;
    std::function<void(TextEdit &, const AcceptedResultCandidate &)>
        attachAcceptedResultCarrier;
    std::function<bool(TextEdit &, llvm::StringRef, llvm::StringRef, uint64_t,
                       uint64_t)>
        authorizeTUIncludeClosure;
    std::function<void()> resetAttemptStats;
  };

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
      RefoldProofLattice &proofLattice,
      const RefoldTheoremAudit &theoremAuditService, RefoldStats &lastStats,
      std::vector<MaterializedEditMapping> *materializedEditMappings,
      Hooks hooks)
      : model_(model), bSource_(bSource), aToks_(aToks),
        abTokHunks_(abTokHunks), abTokMapB2A_(abTokMapB2A),
        abTokAnchorProofs_(abTokAnchorProofs), lineDirs_(lineDirs),
        sourceMapper_(sourceMapper), paths_(paths),
        macroTopology_(macroTopology), lineControlProof_(lineControlProof),
        macroStateProof_(macroStateProof),
        preprocessingStructureIndex_(preprocessingStructureIndex),
        terminalSink_(terminalSink), lexLang_(lexLang),
        includeInsertionPlanner_(includeInsertionPlanner),
        proofLattice_(proofLattice), theoremAuditService_(theoremAuditService),
        lastStats_(lastStats),
        materializedEditMappings_(materializedEditMappings),
        hooks_(std::move(hooks)) {}

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
  RefoldProofLattice &proofLattice_;
  const RefoldTheoremAudit &theoremAuditService_;
  RefoldStats &lastStats_;
  std::vector<MaterializedEditMapping> *materializedEditMappings_;
  Hooks hooks_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEXPANSIONFALLBACKPLANNER_H

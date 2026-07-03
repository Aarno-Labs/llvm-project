//===--- RefoldFinalTUEmissionPlanner.h ------------------------*- C++ -*-===//
//
// Final translation-unit emission planning for clang-refold.
//
// This service owns final TU emission after structural hunk dispatch,
// macro-state repair, and include materialization scheduling have selected the
// concrete edits that may affect the emitted translation unit.  It deliberately
// does not implement low-level edit assembly: pending resync flushing,
// materialized mapping certifying, and accepted-result carrier mechanics remain
// in RefoldTextEditAssembler and the proof services.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDFINALTUEMISSIONPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDFINALTUEMISSIONPLANNER_H

#include "edit/RefoldEditTypes.h"
#include "macro/RefoldMacroStateRepairPlanner.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldIncludeMaterializationScheduler;
class RefoldLineControlProof;
class RefoldMacroTopology;
class RefoldModel;
class RefoldProofLattice;
class RefoldStructuralHunkDispatcher;
class RefoldTerminalProofSink;
class RefoldTextEditAssembler;

/// Plans and emits the final TU text after all structural edit sources have
/// been staged.
class RefoldFinalTUEmissionPlanner {
public:
  /// Services and output ledgers borrowed by final TU emission.
  ///
  /// All pointers are non-owning.  Null pointers are treated as missing service
  /// wiring and cause the planner to fail closed through its normal result path
  /// rather than manufacturing edits without the required proof surface.
  struct Dependencies {
    /// Refold model containing source/include/macro facts.
    const RefoldModel *model = nullptr;
    /// Macro topology service used to interpret staged macro edits.
    const RefoldMacroTopology *macroTopology = nullptr;
    /// Line-control proof service for final observer repairs.
    const RefoldLineControlProof *lineControlProof = nullptr;
    /// Directive inserter used for synthetic line-control text.
    const LineDirectiveInserter *lineDirs = nullptr;
    /// Macro-state repair service whose plan is staged before final emission.
    const RefoldMacroStateRepairPlanner *macroStateRepairPlanner = nullptr;
    /// Final byte-edit assembler used to lower staged edits into TU text.
    const RefoldTextEditAssembler *textEditAssembler = nullptr;
    /// Proof lattice used for accepted-result carrier construction.
    const RefoldProofLattice *proofLattice = nullptr;
    /// Terminal fallback sink for fail-closed emission failures.
    RefoldTerminalProofSink *terminalSink = nullptr;
    /// Optional sidecar mapping output ledger.
    std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr;
    /// Optional final-line-control pruning output ledger.
    std::vector<FinalLineControlPruneCandidate>
        *finalLineControlPruneCandidates = nullptr;
    /// Optional final-line-control source mapping output ledger.
    std::vector<FinalLineControlSourceMapping> *finalLineControlSourceMappings =
        nullptr;
  };

  /// Mutable run state needed to lower the staged structural result into final
  /// TU text.
  struct EmissionRequest {
    /// Translation-unit path used for owner and line-control diagnostics.
    llvm::StringRef tuPath;
    /// Original TU source bytes before final edit assembly.
    llvm::StringRef tuBytes;
    /// Dispatcher containing staged structural edits and token hunk buckets.
    RefoldStructuralHunkDispatcher *structuralHunkDispatcher = nullptr;
    /// Include scheduler containing materialized include state for this pass.
    RefoldIncludeMaterializationScheduler *includeMaterializationScheduler =
        nullptr;
    /// Mutable macro-state repair plan to stage around final emission.
    RefoldMacroStateRepairPlanner::MacroStateRepairPlan *macroStatePlan =
        nullptr;
    /// Immutable macro-state repair request that produced `macroStatePlan`.
    const RefoldMacroStateRepairPlanner::MacroStateRepairRequest
        *macroStateRequest = nullptr;
  };

  /// Result of final TU emission.
  struct EmissionResult {
    /// True when final TU text was emitted without terminal fallback.
    bool success = false;
    /// Complete emitted translation-unit text.
    std::string tuText;
    /// Number of macro roots charged as expanded by surviving final edits.
    size_t expandedMacroCount = 0;
  };

  /// Creates a final TU emission planner bound to the services that own proof,
  /// macro topology, line-control, and byte-edit assembly decisions.
  explicit RefoldFinalTUEmissionPlanner(Dependencies deps);

  /// Stages final TU macro/include edits, applies pending-resync-aware edit
  /// assembly, repairs the TU prologue when preserved file observers require
  /// it, and returns the emitted translation-unit text.
  EmissionResult PlanAndEmit(const EmissionRequest &request) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDFINALTUEMISSIONPLANNER_H

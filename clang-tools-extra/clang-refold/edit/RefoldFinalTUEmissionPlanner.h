//===--- RefoldFinalTUEmissionPlanner.h ------------------------*- C++ -*-===//
//
// Final translation-unit emission planning for clang-refold.
//
// This service owns final TU emission after structural hunk dispatch,
// macro-state repair, and include materialization scheduling have selected the
// concrete edits that may affect the emitted translation unit.  It deliberately
// does not implement low-level edit assembly: pending resync flushing,
// materialized mapping stamping, and accepted-result carrier mechanics remain
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
  /// Services borrowed by final TU emission.
  struct Dependencies {
    const RefoldModel *model = nullptr;
    const RefoldMacroTopology *macroTopology = nullptr;
    const RefoldLineControlProof *lineControlProof = nullptr;
    const LineDirectiveInserter *lineDirs = nullptr;
    const RefoldMacroStateRepairPlanner *macroStateRepairPlanner = nullptr;
    const RefoldTextEditAssembler *textEditAssembler = nullptr;
    const RefoldProofLattice *proofLattice = nullptr;
    RefoldTerminalProofSink *terminalSink = nullptr;
    std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr;
    std::vector<FinalLineControlPruneCandidate> *finalLineControlPruneCandidates =
        nullptr;
    std::vector<FinalLineControlSourceMapping> *finalLineControlSourceMappings =
        nullptr;
  };

  /// Mutable run state needed to lower the staged structural result into final
  /// TU text.
  struct EmissionRequest {
    llvm::StringRef tuPath;
    llvm::StringRef tuBytes;
    RefoldStructuralHunkDispatcher *structuralHunkDispatcher = nullptr;
    RefoldIncludeMaterializationScheduler *includeMaterializationScheduler =
        nullptr;
    RefoldMacroStateRepairPlanner::MacroStateRepairPlan *macroStatePlan =
        nullptr;
    const RefoldMacroStateRepairPlanner::MacroStateRepairRequest
        *macroStateRequest = nullptr;
  };

  /// Result of final TU emission.
  struct EmissionResult {
    bool success = false;
    std::string tuText;
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

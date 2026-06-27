//===--- RefoldFinalTUEmissionPlanner.h ------------------------*- C++ -*-===//
//
// Final translation-unit emission planning for clang-refold.
//
// This service owns the last TU-emission phase after structural hunk dispatch,
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
  /// Services borrowed by the final TU emission phase.
  struct Dependencies {
    const RefoldModel *Model = nullptr;
    const RefoldMacroTopology *MacroTopology = nullptr;
    const RefoldLineControlProof *LineControlProof = nullptr;
    const LineDirectiveInserter *LineDirs = nullptr;
    const RefoldMacroStateRepairPlanner *MacroStateRepairPlanner = nullptr;
    const RefoldTextEditAssembler *TextEditAssembler = nullptr;
    const RefoldProofLattice *ProofLattice = nullptr;
    RefoldTerminalProofSink *TerminalSink = nullptr;
    std::vector<MaterializedEditMapping> *MaterializedEditMappings = nullptr;
    std::vector<FinalLineControlPruneCandidate> *FinalLineControlPruneCandidates =
        nullptr;
    std::vector<FinalLineControlSourceMapping> *FinalLineControlSourceMappings =
        nullptr;
  };

  /// Mutable run state needed to lower the staged structural result into final
  /// TU text.
  struct EmissionRequest {
    llvm::StringRef TUPath;
    llvm::StringRef TUBytes;
    RefoldStructuralHunkDispatcher *StructuralHunkDispatcher = nullptr;
    RefoldIncludeMaterializationScheduler *IncludeMaterializationScheduler =
        nullptr;
    RefoldMacroStateRepairPlanner::MacroStateRepairPlan *MacroStatePlan =
        nullptr;
    const RefoldMacroStateRepairPlanner::MacroStateRepairRequest
        *MacroStateRequest = nullptr;
  };

  /// Result of the final emission phase.
  struct EmissionResult {
    bool Success = false;
    std::string TUText;
    size_t ExpandedMacroCount = 0;
  };

  /// Creates a final TU emission planner bound to the services that own proof,
  /// macro topology, line-control, and byte-edit assembly decisions.
  explicit RefoldFinalTUEmissionPlanner(Dependencies Deps);

  /// Stages final TU macro/include edits, applies pending-resync-aware edit
  /// assembly, repairs the TU prologue when preserved file observers require
  /// it, and returns the emitted translation-unit text.
  EmissionResult PlanAndEmit(const EmissionRequest &Request) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDFINALTUEMISSIONPLANNER_H

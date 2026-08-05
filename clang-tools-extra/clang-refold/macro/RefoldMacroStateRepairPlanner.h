//===--- RefoldMacroStateRepairPlanner.h -----------------------*- C++ -*-===//
//
// Macro-state repair planning for TU-level source edits.
//
// This service owns the orchestration boundary for preserving or materializing
// producer-recorded #define/#undef transitions after final TU edits have been
// staged. The public planner is intentionally thin: policy and shared query
// state live in the private MacroStateRepairContext implementation class in
// RefoldMacroStateRepairPlanner.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROSTATEREPAIRPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROSTATEREPAIRPLANNER_H

#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroPatchPlanner;
class RefoldMacroStateProof;
class RefoldMacroTopology;
class RefoldOwnerStateProof;
class RefoldPathIdentity;
class RefoldProofLattice;
class RefoldStructuralHunkDispatcher;
class RefoldTerminalProofSink;
class RefoldTextEditAssembler;
class RefoldTokenTextAnalysis;

/// Plans macro-state repairs needed after TU edits consume #define/#undef
/// directives while leaving source observers in the final TU stream.
class RefoldMacroStateRepairPlanner {
public:
  struct Dependencies {
    const RefoldModel *model = nullptr;
    const RefoldPathIdentity *pathIdentity = nullptr;
    const RefoldMacroTopology *macroTopology = nullptr;
    const RefoldTokenTextAnalysis *tokenTextAnalysis = nullptr;
    const RefoldMacroStateProof *macroStateProof = nullptr;
    RefoldOwnerStateProof *ownerStateProof = nullptr;
    RefoldProofLattice *proofLattice = nullptr;
    RefoldMacroPatchPlanner *macroPatchPlanner = nullptr;
    RefoldTextEditAssembler *textEditAssembler = nullptr;
    RefoldTerminalProofSink *terminalSink = nullptr;
    const clang::LangOptions *lexLang = nullptr;
  };

  struct MacroStateRepairRequest {
    llvm::StringRef tuPath;
    llvm::StringRef tuBytes;
    RefoldStructuralHunkDispatcher *structuralHunkDispatcher = nullptr;
    std::vector<TextEdit> *tuEdits = nullptr;

    /// Regions the fallback ladder ruled out from keeping their source form.
    ///
    /// An include named here is going to be materialized, so repairing macro
    /// state by deleting its directive is planning against a decision already
    /// taken.  Declining to plan that transition is what leaves materialization
    /// to do the work, and it is a *skip*, not a failure: nothing is refused
    /// downstream and the pass is not abandoned.
    const llvm::DenseSet<uint64_t> *ownersMustExpand = nullptr;
  };

  struct NamedMacroDirectiveRef {
    const RefoldModel::MacroDirective *directive = nullptr;
    llvm::StringRef name;
  };

  struct MacroStateRepairPlan {
    bool success = true;
    bool directiveIndexBuilt = false;
    llvm::DenseMap<uint64_t, NamedMacroDirectiveRef> macroDirectiveById;
    llvm::SmallVector<NamedMacroDirectiveRef, 64> namedMacroDirectives;
    llvm::DenseSet<uint64_t> preservedDefinitionDirectiveIds;
    llvm::DenseSet<uint64_t> syntheticUndefPartitionedDefinitionIds;
  };

  /// Creates a macro-state repair planner bound to the subsystem dependencies
  /// used by each repair context.
  explicit RefoldMacroStateRepairPlanner(Dependencies deps);

  /// Builds the initial macro-state repair plan for the current final TU edit
  /// set and applies the first-pass repair mutations needed before emission.
  MacroStateRepairPlan Plan(const MacroStateRepairRequest &request) const;

  /// Carries preserved gap definitions after replacements once later TU edits
  /// have been staged and their final boundaries are known.
  void CarryObservedGapDefinitionsAfterReplacements(
      MacroStateRepairPlan &plan, const MacroStateRepairRequest &request) const;

  /// Repairs macro definitions consumed by a materialized include replacement
  /// while preserving include ancestry and post-include observers.
  bool RepairConsumedDefinitionsForMaterializedInclude(
      MacroStateRepairPlan &plan, const MacroStateRepairRequest &request,
      const RefoldModel::IncludeItem &materializedInclude,
      uint64_t materializedSiteBegin, uint64_t materializedSiteEnd,
      std::string &replacementText) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROSTATEREPAIRPLANNER_H

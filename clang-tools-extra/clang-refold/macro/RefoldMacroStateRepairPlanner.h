//===--- RefoldMacroStateRepairPlanner.h -----------------------*- C++ -*-===//
//
// Macro-state repair planning for TU-level source edits.
//
// This service owns the orchestration boundary for preserving or materializing
// producer-recorded #define/#undef transitions after final TU edits have been
// staged.  The public planner is intentionally thin: all phase-local policy and
// shared query state live in the private MacroStateRepairContext implementation
// class in RefoldMacroStateRepairPlanner.cpp.
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
    const RefoldModel *Model = nullptr;
    const RefoldPathIdentity *PathIdentity = nullptr;
    const RefoldMacroTopology *MacroTopology = nullptr;
    const RefoldTokenTextAnalysis *TokenTextAnalysis = nullptr;
    const RefoldMacroStateProof *MacroStateProof = nullptr;
    RefoldOwnerStateProof *OwnerStateProof = nullptr;
    RefoldProofLattice *ProofLattice = nullptr;
    RefoldMacroPatchPlanner *MacroPatchPlanner = nullptr;
    RefoldTextEditAssembler *TextEditAssembler = nullptr;
    RefoldTerminalProofSink *TerminalSink = nullptr;
    const clang::LangOptions *LexLang = nullptr;
  };

  struct MacroStateRepairRequest {
    llvm::StringRef TUPath;
    llvm::StringRef TUBytes;
    RefoldStructuralHunkDispatcher *StructuralHunkDispatcher = nullptr;
    std::vector<TextEdit> *TUEdits = nullptr;
  };

  struct NamedMacroDirectiveRef {
    const RefoldModel::MacroDirective *Directive = nullptr;
    llvm::StringRef Name;
  };

  struct MacroStateRepairPlan {
    bool Success = true;
    bool DirectiveIndexBuilt = false;
    llvm::DenseMap<uint64_t, NamedMacroDirectiveRef> MacroDirectiveById;
    llvm::SmallVector<NamedMacroDirectiveRef, 64> NamedMacroDirectives;
    llvm::DenseSet<uint64_t> PreservedDefinitionDirectiveIds;
    llvm::DenseSet<uint64_t> SyntheticUndefPartitionedDefinitionIds;
  };

  /// Creates a macro-state repair planner bound to the extracted subsystem
  /// dependencies used by each repair context.
  explicit RefoldMacroStateRepairPlanner(Dependencies Deps);

  /// Builds the initial macro-state repair plan for the current final TU edit
  /// set and applies the first-pass repair mutations needed before emission.
  MacroStateRepairPlan Plan(const MacroStateRepairRequest &Request) const;

  /// Carries preserved gap definitions after replacements once later TU edits
  /// have been staged and their final boundaries are known.
  void CarryObservedGapDefinitionsAfterReplacements(
      MacroStateRepairPlan &Plan, const MacroStateRepairRequest &Request) const;

  /// Repairs macro definitions consumed by a materialized include replacement
  /// while preserving include ancestry and post-include observers.
  bool RepairConsumedDefinitionsForMaterializedInclude(
      MacroStateRepairPlan &Plan, const MacroStateRepairRequest &Request,
      const RefoldModel::IncludeItem &MaterializedInclude,
      uint64_t MaterializedSiteBegin, uint64_t MaterializedSiteEnd,
      std::string &ReplacementText) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROSTATEREPAIRPLANNER_H

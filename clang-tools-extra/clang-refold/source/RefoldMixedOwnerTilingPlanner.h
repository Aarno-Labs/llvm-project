//===--- RefoldMixedOwnerTilingPlanner.h ------------------------*- C++ -*-===//
//
// Deterministic mixed-owner token-hunk tiling for clang-refold.
//
// RefoldMixedOwnerTilingPlanner owns the normalization pass that may split one
// token-level replacement/deletion hunk into a unique ordered partition of
// TU/include/macro-owned token segments plus proof-only zero-token state-gap
// edges.  The planner borrows the engine-owned witness ledgers and token-hunk
// cache so later accepted-result builders observe the same proof bindings
// without calling back into RefoldEngine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMIXEDOWNERTILINGPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMIXEDOWNERTILINGPLANNER_H

#include "proof/RefoldAcceptedResultTypes.h"
#include "source/DiffAlgorithms.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class LineDirectiveInserter;
class RefoldMacroTopology;
class RefoldModel;
class RefoldOwnerClassifier;
class RefoldOwnerStateProof;
class RefoldPathIdentity;
class RefoldSourceMapper;

/// Normalizes token hunks whose A-side cover crosses independently provable
/// source owners.
class RefoldMixedOwnerTilingPlanner {
public:
  struct Dependencies {
    const RefoldModel &model;
    llvm::StringRef tuPath;
    const RefoldPathIdentity &pathIdentity;
    const LineDirectiveInserter &lineDirs;
    const clang::LangOptions &lexLang;
    const RefoldMacroTopology &macroTopology;
    RefoldSourceMapper &sourceMapper;
    const RefoldOwnerClassifier &ownerClassifier;
    RefoldOwnerStateProof &ownerStateProof;
    std::vector<diffutils::Hunk> &abTokHunks;
    std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses;
    std::vector<MixedOwnerTilingSegmentBinding> &mixedOwnerTilingSegmentBindings;
  };

  struct MixedOwnerTilingPlan {
    /// Token hunks after deterministic mixed-owner splitting.
    std::vector<diffutils::Hunk> hunks;
    /// Number of durable mixed-owner witnesses emitted during this pass.
    size_t mixedOwnerWitnessCount = 0;
    /// Number of emitted token-segment bindings across those witnesses.
    size_t segmentBindingCount = 0;
  };

  explicit RefoldMixedOwnerTilingPlanner(Dependencies deps);

  /// Split eligible token hunks into unique mixed-owner partitions.
  ///
  /// The input vector is consumed by value so the caller can hand off the
  /// current token-diff plan without retaining a stale pre-normalization copy.
  /// The TU bytes are supplied per run because the buffer is loaded inside
  /// RunSinglePassRefold. The planner refreshes the borrowed ABTokHunks cache and witness ledgers as
  /// part of the same deterministic normalization pass.
  MixedOwnerTilingPlan Plan(std::vector<diffutils::Hunk> hunks,
                            llvm::StringRef tuBytes);

private:
  /// Borrowed service graph and output ledgers for one refold engine instance.
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMIXEDOWNERTILINGPLANNER_H

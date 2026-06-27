//===--- RefoldCounterStabilization.h --------------------------*- C++ -*-===//
//
// __COUNTER__ stabilization planning for clang-refold.
//
// This service owns the suffix-forcing policy for counter-sensitive macro
// realizations.  Once an edited or already-expanded `__COUNTER__` occurrence
// stops participating in the replay-time counter sequence, every later
// counter-bearing patchable macro occurrence must also remain expanded so the
// final preprocessed stream preserves B exactly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCOUNTERSTABILIZATION_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCOUNTERSTABILIZATION_H

#include "core/RefoldModel.h"
#include "core/RefoldOwnerClassifier.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"
#include "util/RefoldDenseMapInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

/// Plans forced macro materializations required to keep `__COUNTER__` stable.
///
/// The planner is intentionally read-only and deterministic.  It owns the
/// counter-specific policy and uses RefoldMacroTopology for macro graph,
/// #define-containment, patchable-cover selection, counter-event identity, and
/// selected-patch expandedness.  Owner classification is injected as a named
/// service; no counter policy is routed through ad hoc hook bundles.
class RefoldCounterStabilization {
public:
  RefoldCounterStabilization(const RefoldModel &model,
                             llvm::ArrayRef<PPTok> aToks,
                             llvm::ArrayRef<PPTok> bToks,
                             const RefoldMacroTopology &macroTopology,
                             const RefoldOwnerClassifier &ownerClassifier);

  /// Compute forced expansions caused by direct edits to counter occurrences.
  ///
  /// If any concrete `__COUNTER__` occurrence is edited in B, the edited
  /// occurrence and every later counter occurrence in producer PP order must be
  /// materialized as a suffix.  Each returned request names the smallest
  /// patchable macro callsite that must remain expanded.
  llvm::SmallVector<ForcedMacroPatchRequest, 32>
  ComputeForcedCounterPatches(llvm::StringRef tuPath,
                              llvm::ArrayRef<int64_t> a2b) const;

  /// Compute forced expansions caused by callsites that already remain expanded.
  ///
  /// This catches the case where ordinary hunk attribution selected an expanded
  /// macro realization whose expansion consumes `__COUNTER__`.  The first such
  /// already-expanded occurrence starts the same forced materialization suffix.
  llvm::SmallVector<ForcedMacroPatchRequest, 32>
  ComputeForcedCounterPatchesFromExpandedMacros(
      llvm::StringRef tuPath,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>>
          &macroPatchByOwnerByMacroId) const;

private:
  llvm::SmallVector<CounterOccurrence, 32>
  CollectCounterOccurrences(llvm::StringRef tuPath) const;

  static void SortCounterOccurrences(
      llvm::SmallVectorImpl<CounterOccurrence> &occs);


  const RefoldModel &model_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldOwnerClassifier &ownerClassifier_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCOUNTERSTABILIZATION_H

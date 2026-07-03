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
///
/// A mixed-owner tiling is theorem-facing: it is not merely a transient
/// rechunking detail.  The planner may replace one token-level hunk with a
/// unique ordered partition of ordinary emitted token segments plus zero-token
/// state-gap edges.  Emitted segments continue downstream as ordinary hunks,
/// while durable witnesses and segment bindings let later accepted candidates
/// recover the full ordered proof path without duplicating state-gap edges in
/// the byte-edit stream.
class RefoldMixedOwnerTilingPlanner {
public:
  /// Borrowed services and durable ledgers for mixed-owner tiling.
  struct Dependencies {
    /// Producer model containing owner/source facts.
    const RefoldModel &model;
    /// Translation-unit path for owner and diagnostics.
    llvm::StringRef tuPath;
    /// Path identity service used when owner facts cross files.
    const RefoldPathIdentity &pathIdentity;
    /// Logical line directive model used for source-state checks.
    const LineDirectiveInserter &lineDirs;
    /// Language options for raw lexer trivia checks.
    const clang::LangOptions &lexLang;
    /// Macro topology service for macro-owned segment classification.
    const RefoldMacroTopology &macroTopology;
    /// Source mapper for A/B token and physical source-byte projection.
    RefoldSourceMapper &sourceMapper;
    /// Owner classifier used to assign hunk subranges to owner domains.
    const RefoldOwnerClassifier &ownerClassifier;
    /// Owner-state proof service used to discharge zero-token state gaps.
    RefoldOwnerStateProof &ownerStateProof;
    /// Shared A/B token-hunk cache refreshed after tiling.
    std::vector<diffutils::Hunk> &abTokHunks;
    /// Durable mixed-owner witnesses produced during tiling.
    std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses;
    /// Bindings from emitted A/B token hunks back to durable witness segments.
    std::vector<MixedOwnerTilingSegmentBinding>
        &mixedOwnerTilingSegmentBindings;
  };

  struct MixedOwnerTilingPlan {
    /// A/B token hunks after deterministic mixed-owner splitting.
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
  /// `RunSinglePassRefold`.  The planner refreshes the borrowed `ABTokHunks`
  /// cache, durable mixed-owner witnesses, and emitted segment bindings as one
  /// deterministic normalization pass.  Source gaps are crossed only when they
  /// are explained by modeled zero-token state owners or lexer-ignorable
  /// trivia; otherwise the original hunk remains unsplit for the normal
  /// owner/fallback path.
  MixedOwnerTilingPlan Plan(std::vector<diffutils::Hunk> hunks,
                            llvm::StringRef tuBytes);

private:
  /// Borrowed service graph and output ledgers for one refold engine instance.
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMIXEDOWNERTILINGPLANNER_H

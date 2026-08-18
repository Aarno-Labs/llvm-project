//===--- RefoldMixedOwnerTilingPlanner.h ------------------------*- C++ -*-===//
//
// Deterministic structural token-hunk tiling for clang-refold.
//
// RefoldMixedOwnerTilingPlanner temporarily retains its historical name while
// owning the broader normalization pass that may split one token-level
// replacement/deletion hunk into a unique ordered partition of TU/include/
// macro-owned token segments plus proof-only zero-token state-gap edges.  A
// partition may be required by different realizers for either hunk kind, or by
// protected preprocessing structure between same-realizer segments.  Deletions
// use one shared empty B boundary; replacements require an exact unique B-token
// projection for every canonical source-run boundary.  The planner borrows the
// engine-owned
// witness ledgers and token-hunk cache so later accepted-result builders
// observe the same proof bindings without calling back into RefoldEngine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMIXEDOWNERTILINGPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMIXEDOWNERTILINGPLANNER_H

#include "clang/Basic/LangOptions.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "source/DiffAlgorithms.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroTopology;
class RefoldModel;
class RefoldOwnerClassifier;
class RefoldOwnerStateProof;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndexProvider;
class RefoldSourceMapper;

/// Normalizes token hunks whose A-side cover requires a structural split.
///
/// The legacy mixed-owner case remains unchanged.  In addition, a deletion or
/// replacement hunk may separate adjacent segments with the same realizer only
/// when an
/// exact preprocessing-structure census proves a protected interval in the
/// physical source gap and one merged emitted edit would overlap that interval.
/// The same-realizer path is not discovered by arbitrary subrange search: the
/// planner first visits every A token in order and derives the unique maximal
/// physical source runs separated by byte-complete protected gaps. Missing,
/// duplicate, overlapping, nonmonotone, or lexically inexact mappings make
/// that direct structural path unavailable. Unknown nontrivia in an internal
/// source gap likewise rejects the partition.
/// Every proof-only source owner in such a byte-complete gap is classified
/// `PreservedInPlace`: it emits no replacement, remains disjoint from the
/// emitted segment carriers, and retains its original source order.  Every
/// emitted segment of a deletion partition carries the same empty B boundary
/// `[q,q)`, and the durable witness records both `q` and the complete ordered
/// preserved-state chain.  For a replacement, every interior canonical A-run
/// boundary must satisfy `projectLower(aSplit) == projectUpper(aSplit) ==
/// bSplit`; the resulting B fragments must be monotone and exactly compose the
/// original target envelope.  This typed disposition and boundary theorem permit
/// modeled state transitions to remain physically in place without pretending
/// that they were replayed, repaired, reconstructed, or moved.
///
/// A structural tiling is theorem-facing: it is not merely a transient
/// rechunking detail.  The planner may replace one token-level hunk with a
/// unique ordered partition of ordinary emitted token segments plus zero-token
/// state-gap edges.  Emitted segments continue downstream as ordinary hunks,
/// while durable witnesses and segment bindings let later accepted candidates
/// recover the full ordered proof path without duplicating state-gap edges in
/// the byte-edit stream.
class RefoldMixedOwnerTilingPlanner {
public:
  /// Borrowed services and durable ledgers for structural tiling.
  struct Dependencies {
    /// Producer model containing owner/source facts.
    const RefoldModel &model;
    /// Translation-unit path for owner and diagnostics.
    llvm::StringRef tuPath;
    /// Path identity service used when owner facts cross files.
    const RefoldPathIdentity &pathIdentity;
    /// Macro topology service for macro-owned segment classification.
    const RefoldMacroTopology &macroTopology;
    /// Source mapper for A/B token and physical source-byte projection.
    RefoldSourceMapper &sourceMapper;
    /// Original translation-unit source bytes, used to read the exact spelling
    /// of a directive preserved between two physical source runs.
    llvm::StringRef tuBytes;
    /// Edited preprocessed bytes, used to read the payload whose side of such a
    /// directive is undetermined.
    llvm::StringRef bSource;
    /// Lexer options for classifying that directive and its payload.
    const clang::LangOptions &lexLang;
    /// Owner classifier used to assign hunk subranges to owner domains.
    const RefoldOwnerClassifier &ownerClassifier;
    /// Owner-state proof service used to discharge zero-token state gaps.
    RefoldOwnerStateProof &ownerStateProof;
    /// Shared exact structure-index provider for TU and header occurrences.
    const RefoldPreprocessingStructureIndexProvider
        &preprocessingStructureIndexes;
    /// Shared A/B token-hunk cache refreshed after tiling.
    std::vector<diffutils::Hunk> &abTokHunks;
    /// Durable structural-tiling witnesses produced during tiling.
    std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses;
    /// Bindings from emitted A/B token hunks back to durable witness segments.
    std::vector<MixedOwnerTilingSegmentBinding>
        &mixedOwnerTilingSegmentBindings;
  };

  struct MixedOwnerTilingPlan {
    /// A/B token hunks after deterministic structural splitting.
    std::vector<diffutils::Hunk> hunks;
    /// Number of durable structural-tiling witnesses emitted during this pass.
    size_t mixedOwnerWitnessCount = 0;
    /// Number of emitted token-segment bindings across those witnesses.
    size_t segmentBindingCount = 0;
  };

  explicit RefoldMixedOwnerTilingPlanner(Dependencies deps);

  /// Split eligible token hunks into unique structural partitions.
  ///
  /// The input vector is consumed by value so the caller can hand off the
  /// current token-diff plan without retaining a stale pre-normalization copy.
  /// Physical source bytes and occurrence-local structure indexes come from the
  /// shared provider. The planner refreshes the borrowed `ABTokHunks` cache,
  /// durable structural-tiling witnesses, and emitted segment bindings as one
  /// deterministic normalization pass. `RefoldEngine::PlanTokenDiff()` commits
  /// and validates that complete result before insertion-ledger construction or
  /// owner dispatch may observe a hunk index. Source gaps are crossed only when
  /// they are explained by modeled zero-token state owners or lexer-ignorable
  /// trivia; otherwise the original hunk remains unsplit for the normal
  /// owner/fallback path.
  MixedOwnerTilingPlan Plan(std::vector<diffutils::Hunk> hunks);

private:
  /// Borrowed service graph and output ledgers for one refold engine instance.
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMIXEDOWNERTILINGPLANNER_H

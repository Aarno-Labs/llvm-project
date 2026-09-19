//===--- RefoldStructuralHunkTilingPlanner.h --------------------*- C++ -*-===//
//
// Deterministic structural token-hunk tiling for clang-refold.
//
// RefoldStructuralHunkTilingPlanner owns the normalization pass that may split
// one token-level replacement/deletion hunk into a unique ordered partition of
// TU/include/macro-owned token segments plus proof-only zero-token state-gap
// edges.  A
// partition may be required by different realizers for either hunk kind, or by
// protected preprocessing structure between same-realizer segments.  Deletions
// use one shared empty B boundary; replacements require an exact unique B-token
// projection for every canonical source-run boundary.  The planner borrows the
// engine-owned
// witness ledgers and token-hunk cache so later accepted-result builders
// observe the same proof bindings without calling back into RefoldEngine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALHUNKTILINGPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALHUNKTILINGPLANNER_H

#include "proof/RefoldTilingWitnessTypes.h"
#include "source/RefoldDiffTypes.h"
#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroStateProof;
class RefoldMacroTopology;
class RefoldModel;
class RefoldOwnerClassifier;
class RefoldOwnerStateProof;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndexProvider;
class RefoldSourceMapper;
class RefoldTokenTextAnalysis;
struct SidebandPragmaEdit;
struct SidebandPragmaLinePairing;

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
class RefoldStructuralHunkTilingPlanner {
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
    /// Macro-state proof service.  A preserved `#define`/`#undef` or
    /// `push_macro`/`pop_macro` between two source runs makes the side of the
    /// undetermined payload a macro-observation question, which only the
    /// producer's macro records can answer.
    const RefoldMacroStateProof &macroStateProof;
    /// Raw-token text analysis used by the per-structure gap-crossing proof to
    /// take the payload's directive inventory.
    const RefoldTokenTextAnalysis &tokenText;
    /// Source mapper for A/B token and physical source-byte projection.
    RefoldSourceMapper &sourceMapper;
    /// Original translation-unit source bytes, used to read the exact spelling
    /// of a directive preserved between two physical source runs.
    llvm::StringRef tuBytes;
    /// Edited preprocessed bytes, used to read the payload whose side of such a
    /// directive is undetermined.
    llvm::StringRef bSource;
    /// Where each `#pragma` line printed into A survived in B.  A directive
    /// preserved between two source runs is printed at their seam, so B's copy
    /// of it can fix a seam alignment alone leaves undetermined.
    llvm::ArrayRef<SidebandPragmaLinePairing> sidebandPragmaLinePairings;
    /// Sideband source edits; a replaying one prints B's lines at its own
    /// source position, which the token hunks must agree with.
    llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits;
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
    std::vector<StructuralHunkTilingWitness> &structuralHunkTilingWitnesses;
    /// Bindings from emitted A/B token hunks back to durable witness segments.
    std::vector<StructuralHunkTilingSegmentBinding>
        &structuralHunkTilingSegmentBindings;
  };

  /// A surviving translation-unit `#pragma` directive at A gap `aGap` that B
  /// prints at B gap `bGap`, which no repair of the hunks around the gap can
  /// realize.  `bLo`/`bHi` bound the B gaps the final hunks do realize there.
  struct PrintedPragmaPlacementViolation {
    uint64_t aGap = 0;
    uint64_t bGap = 0;
    uint64_t bLo = 0;
    uint64_t bHi = 0;
  };

  struct StructuralHunkTilingPlan {
    /// A/B token hunks after deterministic structural splitting.
    std::vector<diffutils::Hunk> hunks;
    /// Number of durable structural-tiling witnesses emitted during this pass.
    size_t witnessCount = 0;
    /// Number of emitted token-segment bindings across those witnesses.
    size_t segmentBindingCount = 0;
    /// The first surviving `#pragma` directive the hunks cannot print where
    /// B does, when there is one; see `EnforcePrintedPragmaPlacement`.
    std::optional<PrintedPragmaPlacementViolation> printedPragmaViolation;
  };

  explicit RefoldStructuralHunkTilingPlanner(Dependencies deps);

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
  ///
  /// \p hunks must already be published in the borrowed `ABTokHunks` cache:
  /// the planner projects candidate B envelopes through the shared source
  /// mapper, which reads that cache, so a caller that normalizes the token diff
  /// after it was published must republish it before planning. The precondition
  /// is checked rather than assumed, because the cache it names is the one
  /// coordinate surface a stale entry cannot be detected on later.
  StructuralHunkTilingPlan Plan(std::vector<diffutils::Hunk> hunks);

private:
  /// Publish \p hunks as the finished plan and refresh the borrowed caches.
  ///
  /// Both exits of Plan() end here, so the empty-input early return and the
  /// completed tiling pass report the witness and binding counts the same way.
  StructuralHunkTilingPlan FinishPlan(std::vector<diffutils::Hunk> hunks);

  /// Make the hunks print every printed pragma line whose B position is known
  /// where B prints it, or report the first they cannot.
  ///
  /// A paired line keeps its source, and a replaying sideband edit writes B's
  /// lines at a source position, so each is printed wherever that position
  /// lands among the realized hunks: before a hunk that starts at its A gap,
  /// after one that ends there, and on the side a pure insertion at the gap is
  /// placed on.  The B gap must be one of those.  Pairing cannot guarantee
  /// that -- it is decided before, and independently of, the alignment the
  /// hunks come from -- so it is checked here, on the final hunks.
  ///
  /// A translation-unit `#pragma` directive line may be repaired: when B
  /// prints it inside a hunk bordering its gap, B's token order forces the
  /// tokens on the far side into a pure insertion at the gap, placed by
  /// `placeTUInsertionAmongPrintedPragmas`.  Hunks a tiling witness is bound
  /// to are never changed.
  ///
  /// Every other paired line -- a header directive, a `_Pragma` operator, a
  /// line a macro expansion produced -- and the lines a translation-unit
  /// sideband edit writes are checked only, because the insertion placement
  /// proves an order only against translation-unit directive lines: each must
  /// sit at the single B gap the hunks leave at its A gap.  A line whose macro
  /// caller chain does not resolve has no known carrier, so no hunk may border
  /// its gap.  Every disagreement is returned, and must fail closed.
  ///
  /// This is necessary, not sufficient, for a line whose carrier a realizer
  /// may replace -- a macro invocation rewritten or expanded from B, or a
  /// header materialized around a sideband edit.  Where such a realizer puts
  /// the line is decided after planning and is not checked here.
  std::optional<PrintedPragmaPlacementViolation> EnforcePrintedPragmaPlacement(
      std::vector<diffutils::Hunk> &hunks,
      const std::set<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>>
          &boundHunks) const;

  /// Borrowed service graph and output ledgers for one refold engine instance.
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALHUNKTILINGPLANNER_H

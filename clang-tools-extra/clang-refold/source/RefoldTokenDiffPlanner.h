//===--- RefoldTokenDiffPlanner.h -------------------------------*- C++ -*-===//
//
// Token-diff planning service for clang-refold.
//
// RefoldTokenDiffPlanner owns A/B preprocessed-token alignment. It maps token
// spellings to diff lexemes, builds the owner/provenance
// gap profiles consumed by the certified LCS, derives normalized token hunks,
// and refreshes the byte-hunk caches used by later A/B coordinate projection.
// The service borrows the engine-owned output caches so downstream planners can
// continue to observe the same per-run diff state without calling back into
// RefoldEngine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENDIFFPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENDIFFPLANNER_H

#include "source/DiffAlgorithms.h"
#include "source/RefoldAlignmentSemanticResolver.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace clang {
namespace refold {

struct AlignmentProtectedBoundarySurfaces;
struct AlignmentProtectedBoundaryIdentity;
class RefoldMacroTopology;
class RefoldModel;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndexProvider;
class RefoldSourceMapper;

/// Builds the deterministic A/B preprocessed-token diff plan.
///
/// This service maps `PPTok` streams into stable LCS lexemes, constructs
/// owner/provenance profiles for token gaps, computes the certified A-to-B
/// token map, normalizes insert-only hunks, and refreshes the shared token/byte
/// diff caches.  It stops at the token/LCS boundary; mixed-owner splitting and
/// proof-class selection are separate services.
class RefoldTokenDiffPlanner {
public:
  /// Borrowed token streams, services, and output caches for one diff plan.
  struct Dependencies {
    /// Producer model that supplies owner and token provenance.
    const RefoldModel &model;
    /// Canonical path-equivalence service used by diagnostic source binding.
    const RefoldPathIdentity &pathIdentity;
    /// Shared exact structure-index provider used by the alignment collector
    /// for the TU and every represented concrete header occurrence.
    const RefoldPreprocessingStructureIndexProvider
        &preprocessingStructureIndexes;
    /// Edited preprocessed B source used for byte-hunk projection.
    llvm::StringRef bSource;
    /// Original preprocessed token stream A.
    llvm::ArrayRef<PPTok> aToks;
    /// Edited preprocessed token stream B.
    llvm::ArrayRef<PPTok> bToks;
    /// Byte offsets for A tokens in the original preprocessed A buffer.
    llvm::ArrayRef<size_t> aTokOff;
    /// Byte offsets for B tokens in `bSource`.
    llvm::ArrayRef<size_t> bTokOff;
    /// Macro topology service used to build owner/provenance gap profiles.
    const RefoldMacroTopology &macroTopology;
    /// Source mapper whose byte-diff cache is refreshed by planning.
    RefoldSourceMapper &sourceMapper;
    /// Output A-side PP-gap owner-depth profile consumed by certified LCS.
    std::vector<uint32_t> &ownerDepthGap;
    /// Output normalized A/B token hunks in preprocessed token coordinates.
    std::vector<diffutils::Hunk> &abTokHunks;
    /// Output A/B preprocessed-byte hunk cache, populated when byte projection
    /// succeeds.
    std::optional<std::vector<diffutils::Hunk>> &abByteHunks;
    /// Output A-token to B-token map; unmatched entries are negative.
    std::vector<int64_t> &abTokMapA2B;
    /// Output B-token to A-token map; unmatched entries are negative.
    std::vector<int64_t> &abTokMapB2A;
    /// Output theorem authority parallel to `abTokMapA2B`.
    std::vector<diffutils::LcsAnchorProof> &abTokAnchorProofs;
    /// Internal theorem-bearing map override used only by isolated semantic
    /// simulations. Null for the production run.
    const AlignmentSelectionOverride *alignmentOverride = nullptr;
    /// Production semantic resolver invoked after exact core certification and
    /// before token hunks are published. Empty for isolated simulations.
    ///
    /// The `uint64_t` parameter is the same certification proof budget the
    /// core theorem ran under. Per-window pair facts are recomputed on demand
    /// during resolution, so the resolver must charge that work against the
    /// identical budget rather than an independently chosen one.
    std::function<void(
        llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::StringRef>,
        llvm::ArrayRef<diffutils::LcsAGapProvenance>,
        llvm::ArrayRef<diffutils::LcsBGapProvenance>, uint64_t,
        diffutils::CertifiedLcsResult &)>
        semanticAlignmentResolver;
  };

  /// Result of the deterministic A/B preprocessed-token diff plan.
  struct TokenDiffPlan {
    /// Normalized A/B token-level edit hunks before owner-aware
    /// tiling/splitting.
    std::vector<diffutils::Hunk> hunks;
    /// Structured core-optimal alignment result retained for later semantic
    /// resolution. `alignment.selectedMap` is the certified map used to derive
    /// `hunks`; each retained anchor belongs to a certified window even when
    /// another independent window is uncertified.
    diffutils::CertifiedLcsResult alignment;
  };

  explicit RefoldTokenDiffPlanner(Dependencies deps);

  /// Build the initial token diff and refresh the shared diff caches.
  ///
  /// The plan lexeme-maps both A and B token sequences, computes A-side owner
  /// depth and structured gap provenance, retains the exact core LCS result,
  /// invokes the semantic equivalence resolver when ambiguity remains, derives
  /// normalized token hunks, and builds the raw-byte
  /// hunk/prefix-delta caches used by later coordinate projection. The returned
  /// hunks are identical to
  /// the cached `abTokHunks` at this stage. Later owner-aware normalization
  /// may split the vector, but this service deliberately stops at the
  /// deterministic token/LCS boundary.
  TokenDiffPlan Plan();

private:
  /// Map preprocessor tokens into the stable lexeme sequence consumed by LCS.
  ///
  /// Returned `StringRef`s point into the caller-owned token spellings.  The
  /// offset table is supplied so diagnostics and future provenance checks can
  /// stay aligned with the same token stream; the mapping itself must remain a
  /// pure spellings-to-lexemes transform.
  static std::vector<llvm::StringRef> MapLexemes(llvm::ArrayRef<PPTok> toks,
                                                 llvm::ArrayRef<size_t> offs);

  /// Compute per-gap ownership depth for A-side PP-token gaps.
  ///
  /// The returned vector has one entry for each PP gap, including the sentinel
  /// gap after the last token.  Larger values indicate deeper, more specific
  /// ownership across include and conditional-arm structure, so the LCS
  /// objective can prefer insertion anchors that remain inside the correct
  /// nested owner region instead of drifting outward to shallower boundaries.
  std::vector<uint32_t> ComputeOwnerDepthGapsForPP() const;

  /// Compute structured A-side gap provenance for certified LCS mapping.
  ///
  /// This is the identity-preserving counterpart to owner-depth scoring.  It
  /// records include, conditional-arm, and macro-expansion context on both
  /// sides of each PP-token gap while preserving the scalar owner-depth used by
  /// the core LCS objective. The exact core certifier uses only owner depth; the
  /// identity-bearing fields remain available to downstream structural proofs.
  std::vector<diffutils::LcsAGapProvenance>
  ComputeLcsAGapProvenanceForPP(llvm::ArrayRef<uint32_t> ownerDepthGap) const;

  /// Compute edited-side B-gap line/surface provenance for shadow diagnostics.
  ///
  /// This profile reconstructs the evidence consumed by the known-good legacy
  /// boundary selector. The semantic resolver uses it only in trace mode to
  /// explain historical output; it never participates in production admission.
  std::vector<diffutils::LcsBGapProvenance>
  ComputeLcsBGapProvenanceForPP() const;

  /// Collect independent proof-scheduling and diagnostic boundary surfaces.
  ///
  /// The proof surface contains only the historical sorted, unique direct-TU
  /// A frontiers. The diagnostic surface additionally walks every represented
  /// concrete include occurrence, retaining one canonical identity per
  /// physical construct and role even when several identities project to the
  /// same frontier. Header identities never nominate a new production split.
  /// Unmappable intervals retain an absent A frontier and incomplete projection
  /// state; missing owner source is recorded explicitly rather than replaced
  /// by an invented interval or nearest-token coordinate.
  AlignmentProtectedBoundarySurfaces CollectProtectedAlignmentBoundarySurfaces(
      bool retainDiagnosticIdentities) const;

  /// Emit the canonical physical identity census used by ambiguity records.
  ///
  /// This trace joins stable diagnostic ids back to source paths, concrete
  /// include owners, physical ranges, and exact A projections. It is a pure
  /// serializer and never contributes a boundary to proof scheduling.
  void TraceProtectedAlignmentBoundaryIdentities(
      llvm::ArrayRef<AlignmentProtectedBoundaryIdentity> identities) const;

  /// Serialize completed evidence-only ambiguity-window records.
  ///
  /// This routine performs no theorem reconstruction: it never queries an
  /// oracle, derives forced-anchor windows, enumerates admissible pairs, or
  /// projects protected boundaries. Diagnostic collection is completed before
  /// this serializer is called and cannot affect alignment authority.
  void TraceAlignmentAmbiguityWindows(
      llvm::ArrayRef<llvm::StringRef> aSeq,
      llvm::ArrayRef<diffutils::LcsAmbiguityWindowDiagnosticRecord> windows,
      llvm::ArrayRef<AlignmentProtectedBoundaryIdentity> identities) const;

  /// Emit the stable evidence-only transcript retained by the certifier.
  void TraceAlignmentCertificationRun(
      const diffutils::CertifiedLcsResult &alignment,
      const diffutils::LcsCertificationDiagnosticEvidence &diagnosticEvidence,
      uint64_t aTokenCount, uint64_t bTokenCount) const;

  /// Borrowed construction-time service graph for token-diff planning.
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENDIFFPLANNER_H

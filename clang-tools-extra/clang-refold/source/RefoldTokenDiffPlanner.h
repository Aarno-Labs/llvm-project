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
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroTopology;
class RefoldModel;
class RefoldSourceMapper;

class RefoldTokenDiffPlanner {
public:
  struct Dependencies {
    const RefoldModel &model;
    llvm::StringRef bSource;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    llvm::ArrayRef<size_t> aTokOff;
    llvm::ArrayRef<size_t> bTokOff;
    const RefoldMacroTopology &macroTopology;
    RefoldSourceMapper &sourceMapper;
    std::vector<uint32_t> &ownerDepthGap;
    std::vector<diffutils::Hunk> &abTokHunks;
    std::optional<std::vector<diffutils::Hunk>> &abByteHunks;
    std::vector<int64_t> &abTokMapA2B;
    std::vector<int64_t> &abTokMapB2A;
  };

  struct TokenDiffPlan {
    /// Normalized token-level edit hunks before owner-aware tiling/splitting.
    std::vector<diffutils::Hunk> hunks;
  };

  explicit RefoldTokenDiffPlanner(Dependencies deps);

  /// Build the initial token diff and refresh the engine-owned diff caches.
  ///
  /// The returned hunks are identical to the cached ABTokHunks produced here.
  /// Later owner-aware normalization may split that vector further, but this
  /// service deliberately stops at the deterministic token/LCS boundary.
  TokenDiffPlan Plan();

private:
  /// Map preprocessor tokens into the stable lexeme sequence consumed by LCS.
  static std::vector<llvm::StringRef>
  MapLexemes(llvm::ArrayRef<PPTok> toks, llvm::ArrayRef<size_t> offs);

  /// Compute per-gap owner depth for the A-side token stream.
  std::vector<uint32_t> ComputeOwnerDepthGapsForPP() const;

  /// Compute structured A-side gap provenance for certified LCS mapping.
  std::vector<diffutils::LcsAGapProvenance>
  ComputeLcsAGapProvenanceForPP(llvm::ArrayRef<uint32_t> ownerDepthGap) const;

  /// Compute edited-side source-surface profiles for B-side token gaps.
  std::vector<diffutils::LcsBGapProvenance>
  ComputeLcsBGapProvenanceForPP() const;

  /// Borrowed construction-time service graph for token-diff planning.
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENDIFFPLANNER_H

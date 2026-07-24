//===--- RefoldLegacyAlignmentDiagnostic.h ---------------------*- C++ -*-===//
//
// Boundary proposal reconstructed from the pre-semantic-resolver policy.
//
// This service reconstructs the last regression-passing Patch 6 partial map
// from the exact all-optimal oracle. The historical ranks are proposal-only:
// the semantic resolver must independently prove every non-forced anchor by
// counterfactual planning or realized-source equivalence before it can become
// production authority.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLEGACYALIGNMENTDIAGNOSTIC_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLEGACYALIGNMENTDIAGNOSTIC_H

#include "source/DiffAlgorithms.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace clang {
namespace refold {

/// Historical source of one Patch 6 shadow anchor.
enum class LegacyAlignmentAnchorOrigin : uint8_t {
  None,
  CoreForced,
  BidirectionallyUniqueAdmissible,
  BoundaryPureRestoration,
  SuffixInsertionRestoration,
};

/// Proposal reconstructed from the Patch 6 selected alignment policy.
struct LegacyAlignmentDiagnosticResult {
  std::vector<int64_t> selectedMap;
  std::vector<LegacyAlignmentAnchorOrigin> anchorOrigins;
  std::vector<diffutils::Hunk> hunks;

  uint64_t coreForcedAnchorCount = 0;
  uint64_t uniquePartnerAnchorCount = 0;
  uint64_t boundaryPureAnchorCount = 0;
  uint64_t suffixInsertionAnchorCount = 0;
  uint64_t crossingAnchorSuppressions = 0;

  bool complete = false;
  std::string constructionFailure;
  bool monotone = false;
  bool lexemesAgree = false;

  /// True only when one globally optimal core path contains every selected
  /// shadow anchor simultaneously.  Historical Patch 6 admitted anchors
  /// individually, so this can be false even when every anchor occurs on some
  /// optimal path.
  bool jointlyCoreOptimal = false;
  std::string jointOptimalityFailure;
};

/// Reconstruct the exact Patch 6 boundary-restoration proposal.
///
/// The implementation intentionally preserves the historical ranking and
/// tie-handling rules byte-for-byte in meaning. The returned map is not itself
/// authority; it identifies the exact anchors that the semantic resolver must
/// validate through complete counterfactual and realization theorems.
LegacyAlignmentDiagnosticResult reconstructLegacyBoundaryProposal(
    llvm::ArrayRef<llvm::StringRef> aLexemes,
    llvm::ArrayRef<llvm::StringRef> bLexemes,
    llvm::ArrayRef<diffutils::LcsAGapProvenance> aGapProvenance,
    llvm::ArrayRef<diffutils::LcsBGapProvenance> bGapProvenance,
    const diffutils::CertifiedLcsResult &coreAlignment);

/// Return a stable name for trace output.
llvm::StringRef toString(LegacyAlignmentAnchorOrigin origin);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLEGACYALIGNMENTDIAGNOSTIC_H

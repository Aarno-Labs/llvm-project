//===--- RefoldAlignmentSemanticEvidence.h -----------------------*- C++ -*-===//
//
// Exact source-preservation facts for semantic alignment resolution.
//
// These records describe the source carriers and preprocessing owners changed
// by one isolated alignment simulation. The resolver may use exact set
// inclusion only as a counterfactual theorem; numerical size or proximity
// rankings are never admission criteria.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTSEMANTICEVIDENCE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTSEMANTICEVIDENCE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

/// Coordinate domain for one staged source mutation.
enum class AlignmentSemanticMutationDomain : uint8_t {
  TUBytes,
  IncludeTokens,
  IncludeSourceBytes,
  MacroInvocationTokens,
  ProtectedTUBytes,
};

/// One exact source carrier changed by an isolated alignment simulation.
struct AlignmentSemanticMutationRecord {
  AlignmentSemanticMutationDomain domain =
      AlignmentSemanticMutationDomain::TUBytes;
  std::optional<uint64_t> ownerIncludeId;
  uint64_t ownerId = 0;
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t replacementBytes = 0;
  bool protectedStructure = false;

  /// Exact original bytes for this carrier when its coordinate domain is a
  /// physical source-byte range available to the isolated simulation.
  std::string originalText;
  /// Exact replacement bytes emitted for this carrier. Protected-structure
  /// authorization records leave this empty because they are capabilities,
  /// not independent source transformations.
  std::string replacementText;
  /// True only when `originalText` and `replacementText` completely describe
  /// the byte transformation over `[begin, end)`.
  bool hasExactByteTransformation = false;

  bool IsInsertion() const { return begin == end; }
};

/// Exact preservation footprint retained for theorem comparison.
struct AlignmentSemanticPreservationFootprint {
  std::vector<AlignmentSemanticMutationRecord> mutations;
  std::vector<uint64_t> expandedIncludeIds;
  std::vector<uint64_t> expandedMacroRootIds;

  /// Suffix/observer/counter/composition postconditions, excluding raw target
  /// hunk identity and source-carrier identity.
  std::string semanticPostconditionKey;
  /// The same postcondition plus producer, boundary, and diagnostic classes.
  std::string realizationPostconditionKey;
  bool semanticPostconditionComplete = true;
  bool realizationPostconditionComplete = true;
  std::string incompletePostconditionDimensions;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTSEMANTICEVIDENCE_H

//===--- RefoldFinalAssemblyVerifier.h -------------------------*- C++ -*-===//
//
// Closing soundness check for one assembled refold result.
//
// Every theorem in the refolder states which B tokens its candidate realizes,
// and selection trusts that claim: candidates are pooled into an equivalence
// class by the range they claim and then ranked by cost.  A claim that is not
// met therefore outranks a correct realization instead of being rejected, and
// the wrong text reaches the output.  Re-preprocessing the finished assembly is
// the only check that does not rest on the same claims it is meant to police.
//
// The relation checked here is deliberately the one `--check` already enforces
// in production: *both* sides are preprocessed, so the comparison is between
// `preprocess(assembly)` and `preprocess(edited stream)`.  Comparing the
// assembly against the edited stream *as written* would be a stronger and wrong
// relation -- the edited stream is not required to be a fixed point of the
// preprocessor, and in practice carries comments and spacing that no replay can
// reproduce.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDFINALASSEMBLYVERIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDFINALASSEMBLYVERIFIER_H

#include "core/RefoldModel.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

/// Result of checking one assembled final source against the edited stream.
struct FinalAssemblyVerdict {
  /// True when the assembly replays the edited stream under the run's mode.
  bool verified = false;

  /// True when the check could not be run at all -- the preprocessor could not
  /// be invoked, for instance.
  ///
  /// Inconclusive is never a rejection.  An assembly this check cannot read is
  /// left to the proof paths that produced it, exactly as it would be if the
  /// check did not exist; condemning it would trade a real refold for a missing
  /// measurement.
  bool inconclusive = false;

  /// First divergence, described in the same terms `--check` reports.  Empty
  /// unless the assembly was rejected.
  std::string reason;

  /// Index of the first diverging token in the preprocessed edited stream.
  /// Only meaningful when the assembly was rejected; this is the anchor a
  /// caller uses to find the region responsible.
  std::size_t mismatchTokenIndex = 0;
};

/// Checks assembled final sources against the edited preprocessed stream.
///
/// The edited stream is preprocessed once at construction and reused, so
/// repeated checks -- a caller narrowing an unsound region and re-assembling,
/// say -- pay for one preprocess per assembly rather than two.
class RefoldFinalAssemblyVerifier {
public:
  /// Build a verifier for one refold run.
  ///
  /// \param rootJson producer refold map, used only to build relaxation masks
  /// \param ctx producer preprocessing context, replayed for both sides
  /// \param editedStreamBytes the edited preprocessed stream, as written
  /// \param noLines whether the run prunes line directives
  /// \param strict whether the run is byte-exact about stringified operands
  /// \param scratchNeighborPath a path whose directory hosts the temporary
  ///        sources; using one directory for both sides keeps `__FILE__` and
  ///        quoted-include lookup comparable between them
  ///
  /// Returns nullopt when the edited stream cannot be preprocessed, which
  /// leaves the caller with no verifier rather than a failing one.
  static std::optional<RefoldFinalAssemblyVerifier>
  Create(const llvm::json::Object &rootJson,
         const RefoldModel::PreprocessContext &ctx,
         llvm::StringRef editedStreamBytes, bool noLines, bool strict,
         llvm::StringRef scratchNeighborPath);

  /// Check one assembled final source.
  FinalAssemblyVerdict Verify(llvm::StringRef finalSource) const;

  /// Preprocessed edited-stream tokens, for callers that need to locate the
  /// region owning a reported mismatch.
  llvm::ArrayRef<PPTok> EditedTokens() const { return editedTokens_; }

private:
  RefoldFinalAssemblyVerifier() = default;

  RefoldModel::PreprocessContext ctx_;
  std::string scratchNeighborPath_;

  /// Edited stream after preprocessing -- the side of the comparison that is
  /// fixed for the lifetime of this verifier.
  std::vector<PPTok> editedTokens_;
  std::vector<std::size_t> editedTokenOffsets_;

  /// Per-token relaxations, in edited-token indexing.  Empty when the run's
  /// mode admits no relaxation, in which case comparison is exact.
  std::vector<std::uint8_t> ignoreMask_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDFINALASSEMBLYVERIFIER_H

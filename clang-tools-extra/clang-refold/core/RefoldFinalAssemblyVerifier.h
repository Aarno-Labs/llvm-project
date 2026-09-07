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
#include <utility>
#include <string>
#include <vector>

namespace clang {
namespace refold {

/// What the closing output check does with its verdict.
///
/// The check re-preprocesses the finished assembly and compares it to the
/// edited stream.  It costs one preprocess of each side, so it is requested
/// rather than always paid for, and what to do about a divergence is a policy
/// the caller owns.
enum class OutputVerificationMode {
  /// Do not build or run the check.  Neither side is preprocessed, and a
  /// theorem that mis-states what it realizes is not detected here.
  Off,

  /// Run the check and repair a divergence: rule out preserving the smallest
  /// region that owns it, re-assemble, and repeat until the result replays or
  /// nothing narrower is left.
  ///
  /// A verdict this mode cannot reach at all is kept rather than repaired: an
  /// inconclusive check names no region to rule out, so there is nothing to
  /// re-assemble around, and rejecting would trade a real refold for a
  /// missing measurement rather than for a proven defect.
  Repair,

  /// Run the check and fail the refold unless it verifies.  A divergence
  /// fails because repairing silently would cost completeness in a way
  /// nothing observes -- the output stays correct, so the defective theorem
  /// survives.  An inconclusive check fails too: this mode asks for a
  /// verified assembly, and a comparison that could not be performed does not
  /// produce one.
  Fatal
};

/// What one closing check established about an assembly.
///
/// The three outcomes are mutually exclusive by construction.  A comparison
/// that could not be performed is its own answer and must not be spelled as a
/// positive result: "I could not check" is not "it checked out", and a caller
/// that reads only a `verified` flag would not be able to tell them apart.
enum class FinalAssemblyVerdictKind : uint8_t {
  /// The assembly replays the edited stream under the run's mode.
  Verified,

  /// The assembly and the edited stream were both preprocessed and they
  /// differ.  This is the only outcome carrying divergence evidence.
  Diverged,

  /// The check could not be run at all -- the assembly could not be
  /// preprocessed, for instance.
  ///
  /// Inconclusive is not a rejection under `Repair`: an assembly this check
  /// cannot read names no region to narrow, so it is left to the proof paths
  /// that produced it, exactly as it would be if the check did not exist, and
  /// condemning it would trade a real refold for a missing measurement.
  Inconclusive
};

/// What a run must do with one closing-check verdict.
enum class FinalAssemblyDisposition : uint8_t {
  /// Take the assembly as it stands.
  Accept,

  /// Rule out preserving the smallest region owning the divergence and
  /// re-assemble.  Only a `Diverged` verdict names such a region.
  Repair,

  /// Fail the refold.
  Fail
};

/// Return what \p mode requires for \p kind.
///
/// The option's whole contract is this one table, so a caller cannot state
/// half of it.  Two cells carry the reasoning:
///
///   * `Repair` + `Inconclusive` accepts.  An unperformed comparison names no
///     region to rule out, so there is nothing to re-assemble around, and
///     rejecting would trade a real refold for a missing measurement rather
///     than for a proven defect.
///   * `Fatal` + `Inconclusive` fails.  That mode asks for an assembly the
///     check has verified, and a comparison that could not be performed does
///     not produce one.  This is the option's contract rather than a
///     soundness claim: the check is defense in depth, and its absence leaves
///     the proof paths exactly as they would be with the check off.
constexpr FinalAssemblyDisposition
DispositionForVerdict(OutputVerificationMode mode,
                      FinalAssemblyVerdictKind kind) {
  if (kind == FinalAssemblyVerdictKind::Verified)
    return FinalAssemblyDisposition::Accept;

  switch (mode) {
  case OutputVerificationMode::Off:
    // The check is not built in this mode, so no verdict exists to dispose
    // of.  Answer for completeness of the table, not because it is reached.
    return FinalAssemblyDisposition::Accept;
  case OutputVerificationMode::Repair:
    return kind == FinalAssemblyVerdictKind::Diverged
               ? FinalAssemblyDisposition::Repair
               : FinalAssemblyDisposition::Accept;
  case OutputVerificationMode::Fatal:
    return FinalAssemblyDisposition::Fail;
  }
  return FinalAssemblyDisposition::Accept;
}

/// Result of checking one assembled final source against the edited stream.
struct FinalAssemblyVerdict {
  /// Which of the three outcomes this check reached.  Defaults to
  /// `Inconclusive` so a verdict that was never filled in claims nothing.
  FinalAssemblyVerdictKind kind = FinalAssemblyVerdictKind::Inconclusive;

  /// First divergence, described in the same terms `--check` reports.  Empty
  /// unless `kind` is `Diverged`.
  std::string reason;

  /// Index of the first diverging token in the preprocessed edited stream.
  /// Only meaningful when `kind` is `Diverged`; this is the anchor a caller
  /// uses to find the region responsible.
  std::size_t mismatchTokenIndex = 0;

  /// Every diverging run, as half-open ranges of preprocessed edited-stream
  /// token indices, in ascending order.  Empty unless `kind` is `Diverged`.
  ///
  /// Reporting only the first divergence forces the caller to re-assemble once
  /// per diverging region, which is one whole refold per region and, past any
  /// attempt ceiling, reaches the conservative carrier with work still
  /// available.  Every region is reported so a caller can give all of them up
  /// in one attempt; what remains for a second attempt is widening, which is
  /// bounded by nesting depth rather than by the number of divergences.
  std::vector<std::pair<std::size_t, std::size_t>> divergentRanges;
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
  /// \param scratchNeighborPath the *producer's* source path, whose directory
  ///        hosts the temporary sources.  It must not be the refold output
  ///        path: a quoted include resolves against the including file's own
  ///        directory before any `-I`, so preprocessing beside an output
  ///        directory that holds a later copy of the same header answers the
  ///        check against headers the producer never read.  See
  ///        `producerSourceAnchorPath()`.
  /// \param verifyIncludeDirs caller-declared last-resort include directories
  ///        for headers the edit introduced, which no producer-recorded search
  ///        path can find; see `buildFinalSourcePreprocessCallback()`
  ///
  /// Returns nullopt when the edited stream cannot be preprocessed, which
  /// leaves the caller with no verifier rather than a failing one.
  static std::optional<RefoldFinalAssemblyVerifier>
  Create(const llvm::json::Object &rootJson,
         const RefoldModel::PreprocessContext &ctx,
         llvm::StringRef editedStreamBytes, bool noLines, bool strict,
         llvm::StringRef scratchNeighborPath,
         llvm::ArrayRef<std::string> verifyIncludeDirs);

  /// Check one assembled final source.
  FinalAssemblyVerdict Verify(llvm::StringRef finalSource) const;

  /// Preprocessed edited-stream tokens, for callers that need to locate the
  /// region owning a reported mismatch.
  llvm::ArrayRef<PPTok> EditedTokens() const { return editedTokens_; }

private:
  RefoldFinalAssemblyVerifier() = default;

  /// Append every diverging run, in ascending edited-stream token order.
  void AppendDivergentRanges(
      llvm::ArrayRef<PPTok> assemblyTokens,
      std::vector<std::pair<std::size_t, std::size_t>> &ranges) const;

  RefoldModel::PreprocessContext ctx_;
  std::string scratchNeighborPath_;
  /// Caller-declared last-resort include directories, replayed on every check.
  std::vector<std::string> verifyIncludeDirs_;

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

//===--- RefoldBInsertionLedger.h ------------------------------*- C++ -*-===//
//
// B-token pure-insertion claim ledger for clang-refold.
//
// This service owns the run-scoped provenance and claim state for token-level
// pure insertions in the edited preprocessed stream B.  It enforces the global
// invariant that a B-only insertion segment is emitted at most once by letting
// standalone boundary emission pre-claim insertions and letting macro
// whole-cover replay slice around those claims.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDBINSERTIONLEDGER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_EDIT_REFOLDBINSERTIONLEDGER_H

#include "proof/RefoldTheoremTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroBoundarySelector;
class RefoldMacroTopology;
class RefoldOwnerClassifier;
class RefoldSourceMapper;

/// Owns B-token pure-insertion provenance and claim clipping for one refold
/// run.
class RefoldBInsertionLedger {
public:
  /// Named service dependencies needed for insertion ownership and B slicing.
  struct Deps {
    /// Edited preprocessed B-token stream used for insertion slicing.
    llvm::ArrayRef<PPTok> bTokens;
    /// Source mapper used to recover B-token byte slices.
    const RefoldSourceMapper &sourceMapper;
    const RefoldOwnerClassifier &ownerClassifier;
    const RefoldMacroTopology &macroTopology;
    const RefoldMacroBoundarySelector &macroBoundarySelector;
  };

  explicit RefoldBInsertionLedger(Deps deps);

  /// \brief Build provenance indices for token-level pure insertion hunks.
  ///
  /// Scans \p hunks for token-level pure insertions (A-span empty, B-span
  /// non-empty) and populates:
  ///   - \c bInsertions_           : insertion records in B token space
  ///   - \c bTokToInsertionId_     : per-B-token map to insertion id (or -1)
  ///   - \c hunkToInsertionId_     : per-hunk map to insertion id (or -1)
  ///
  /// The resulting structures allow later proof passes to enforce a global
  /// “emit each B-only segment exactly once” invariant (no double-emission).
  void BuildProvenance(llvm::ArrayRef<diffutils::Hunk> hunks);

  /// \brief Pre-claim insertions that must be emitted as standalone boundary
  /// patches.
  ///
  /// Runs as an early planning pass (before macro patch construction) so that
  /// later B-token slicing codepaths (e.g. macro whole-cover replacement) can
  /// clip away B-only segments that are already committed to standalone
  /// emission.
  ///
  /// Macro call-sites take priority: if an insertion lies within a patchable
  /// macro cover, it is left unclaimed so the macro patch may absorb it.
  void PreclaimStandaloneInsertions(llvm::StringRef tuPath,
                                    llvm::ArrayRef<diffutils::Hunk> hunks);

  /// \brief Claim a pure insertion hunk for a specific emission site.
  ///
  /// Claims are used to prevent double-emission. Attempting to claim an already
  /// claimed insertion with a different claim kind is a hard error.
  ///
  /// \param insId Insertion id in \c bInsertions_.
  /// \param claim Claim kind to record.
  /// \param why Debug string describing the claimant (for diagnostics).
  void Claim(size_t insId, BInsertionClaim claim, llvm::StringRef why);

  /// \brief Partition a B-token interval into segments that are safe to emit.
  ///
  /// Returns a small list of sub-ranges of \c [bTokStart,bTokEnd) with any
  /// B-only insertion hunks claimed as \c Standalone removed. The returned
  /// sub-ranges are ordered and non-overlapping.
  llvm::SmallVector<std::pair<size_t, size_t>, 4>
  ClipBTokenRangeAgainstClaims(size_t bTokStart, size_t bTokEnd) const;

  /// \brief Slice \c [bTokStart,bTokEnd) from B while omitting claimed
  /// insertion segments.
  ///
  /// This concatenates the B-token segments returned by \c
  /// ClipBTokenRangeAgainstClaims and returns the resulting B-source byte
  /// sequence.
  std::string SliceBSourceClippedAgainstClaims(size_t bTokStart,
                                               size_t bTokEnd) const;

  /// Return all tracked pure-insertion segments.
  const std::vector<BInsertionProv> &Insertions() const { return bInsertions_; }

  /// Return the B-token reverse insertion index.
  const std::vector<int32_t> &BTokToInsertionId() const {
    return bTokToInsertionId_;
  }

private:
  Deps deps_;
  std::vector<BInsertionProv> bInsertions_;
  std::vector<int32_t> bTokToInsertionId_;
  std::vector<int32_t> hunkToInsertionId_;
};

} // namespace refold
} // namespace clang

#endif

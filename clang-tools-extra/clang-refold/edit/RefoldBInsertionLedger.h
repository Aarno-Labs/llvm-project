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

#include "proof/RefoldAcceptedResultTypes.h"
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

/// Owns B-token pure-insertion provenance and claim clipping for one refold run.
class RefoldBInsertionLedger {
public:
  /// Named service dependencies needed for insertion ownership and B slicing.
  struct Deps {
    llvm::ArrayRef<PPTok> BTokens;
    const RefoldSourceMapper &SourceMapper;
    const RefoldOwnerClassifier &OwnerClassifier;
    const RefoldMacroTopology &MacroTopology;
    const RefoldMacroBoundarySelector &MacroBoundarySelector;
  };

  explicit RefoldBInsertionLedger(Deps deps);

  /// Build provenance indices for token-level pure insertion hunks.
  void BuildProvenance(llvm::ArrayRef<diffutils::Hunk> hunks);

  /// Pre-claim insertions that must be emitted as standalone boundary patches.
  void PreclaimStandaloneInsertions(llvm::StringRef tuPath,
                                    llvm::ArrayRef<diffutils::Hunk> hunks);

  /// Claim a pure insertion hunk for a specific emission site.
  void Claim(size_t insId, BInsertionClaim claim, llvm::StringRef why);

  /// Partition a B-token interval into subranges that are safe to emit.
  llvm::SmallVector<std::pair<size_t, size_t>, 4>
  ClipBTokenRangeAgainstClaims(size_t bTokStart, size_t bTokEnd) const;

  /// Slice B source while omitting Standalone-claimed insertion segments.
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

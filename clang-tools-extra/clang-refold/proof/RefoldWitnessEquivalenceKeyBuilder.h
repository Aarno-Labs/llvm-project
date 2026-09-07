//===--- RefoldWitnessEquivalenceKeyBuilder.h ------------------*- C++ -*-===//
//
// Witness equivalence-key construction service for clang-refold.
//
// The equivalence key partitions accepted-result candidates into resolver-
// comparable equivalence classes by combining several proof dimensions:
//
//   * target preprocessed-token signature (the B-token slice the candidate
//     claims to realize),
//   * suffix-state signature (counter, line-control, macro-state events the
//     candidate's witnesses cover),
//   * preserved-observer signature (line/file/counter/include observers that
//     survive the candidate unchanged),
//   * counter-state signature,
//   * producer kinds,
//   * boundary class, diagnostic class, composition class.
//
// Each dimension is recorded as a deterministic string so that strict-mode
// comparisons across candidate families remain reproducible.  Dimensions that
// are not yet provable for a candidate are recorded as Unknown with a stable
// reason string so the resolver can attribute the missing dimension to a
// specific obligation.
//
// The builder is a pure function of the candidate, the source mapper,
// and the B-token stream; it holds no per-attempt state.  It has no
// back-reference to `RefoldProofLattice`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSEQUIVALENCEKEYBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSEQUIVALENCEKEYBUILDER_H

#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"

namespace clang {
namespace refold {

class RefoldSourceMapper;

/// Build deterministic equivalence keys for accepted-result candidates.
///
/// The builder is read-only: it only inspects the candidate, the configured
/// source mapper (for B-token byte slicing), and the B-token stream length.
/// Two candidates with the same equivalence key are interchangeable from the
/// resolver's point of view; the resolver uses canonical ranking to pick
/// among them.
class RefoldWitnessEquivalenceKeyBuilder {
public:
  /// Borrowed inputs required to build a key.  Every reference must outlive
  /// the builder; the lattice owns all three today.
  struct Dependencies {
    /// Source mapper used to slice B-side bytes when hashing target-PP
    /// token ranges and macro-repair / replay state-neutral signatures.
    const RefoldSourceMapper &sourceMapper;
    /// B-side preprocessed source bytes; used as the target-PP signature
    /// payload for the explicit terminal-fallback key path.
    llvm::StringRef bSource;
    /// B-token stream; used to bound target-PP token ranges and to gate
    /// the macro-repair / replay state-neutral key path.
    llvm::ArrayRef<PPTok> bToks;
  };

  explicit RefoldWitnessEquivalenceKeyBuilder(Dependencies deps);

  /// Build the witness equivalence key for an accepted-result candidate.
  ::clang::refold::WitnessEquivalenceKey
  Build(const AcceptedResultCandidate &candidate) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSEQUIVALENCEKEYBUILDER_H

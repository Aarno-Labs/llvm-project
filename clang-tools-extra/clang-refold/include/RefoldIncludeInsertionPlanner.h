//===--- RefoldIncludeInsertionPlanner.h ------------------------*- C++ -*-===//
//
// Include-owned insertion patch construction for clang-refold.
//
// RefoldIncludeInsertionPlanner builds include-owned staging patches from
// already-attributed diff hunks and resolves theorem-facing B-token envelopes
// for inline include realization.  It borrows immutable B-side token/source
// inputs and the source/proof services needed to certify include-realization
// evidence; it owns no per-run mutable state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEINSERTIONPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEINSERTIONPLANNER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

class RefoldProofLattice;
class RefoldSourceMapper;

/// Builds include-owned insertion patches and include-realization
/// B-token envelopes.
///
/// This service is intentionally standalone from RefoldIncludeMaterializer: it
/// has a narrow dependency surface and can be shared by normal structural
/// staging, include materialization, and terminal fallback planning without
/// making those systems include each other's orchestration surfaces.
class RefoldIncludeInsertionPlanner {
public:
  RefoldIncludeInsertionPlanner(llvm::StringRef bSource,
                                llvm::ArrayRef<PPTok> bToks,
                                llvm::ArrayRef<size_t> bTokOff,
                                const RefoldSourceMapper &sourceMapper,
                                const RefoldProofLattice &proofLattice);

  /// Resolve an include-realization B-token envelope from an A-token cover.
  ///
  /// The canonical path is the ordinary A-cover -> B-envelope mapper.  If that
  /// proof is unavailable, this method permits exactly the declared
  /// BoundaryStableConsensusBCoverEnvelope proof: all usable non-canonical
  /// boundary-stable projections must agree on the same non-empty B-token
  /// envelope.  Missing or empty projections are ignored, but conflicting
  /// usable projections fail closed.
  std::optional<std::pair<size_t, size_t>>
  ResolveIncludeRealizationBTokenEnvelope(
      uint64_t beginTok, uint64_t endTok,
      IncludeRealizationEvidenceKind *evidenceKind = nullptr) const;

  /// Build an include-owned staging patch from an already-attributed diff hunk.
  ///
  /// The returned IncludePatch carries the A/B token intervals from \p h and
  /// the exact bytes copied from B.  Replacement/deletion hunks use exact token
  /// coverage so surrounding inter-token whitespace stays with the neighboring
  /// owner; pure insertions keep the full token envelope because there is no
  /// original source span whose boundary whitespace can be retained.
  IncludePatch BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                          const diffutils::Hunk &h) const;

private:
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> bTokOff_;
  const RefoldSourceMapper &sourceMapper_;
  const RefoldProofLattice &proofLattice_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEINSERTIONPLANNER_H

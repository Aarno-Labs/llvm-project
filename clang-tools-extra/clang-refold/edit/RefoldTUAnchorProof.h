//===--- RefoldTUAnchorProof.h -------------------------------*- C++ -*-===//
//
// TU-anchor accepted-result proof construction for clang-refold.
//
// This service owns only the normalized AcceptedResultCandidate construction
// for translation-unit insertion anchors.  It deliberately does not find
// anchors, classify owners, plan TU byte spans, or participate in
// accepted-result selection.  Keeping this builder separate lets
// RefoldTUEditPlanner mint the same TU-anchor proof carriers without depending
// on RefoldProofLattice; the summary/contract construction itself is the
// shared RefoldProofSummaryBuilder, which is a leaf service and creates no
// planner/lattice cycle.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUANCHORPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUANCHORPROOF_H

#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <utility>

namespace clang {
namespace refold {

class RefoldTheoremAudit;

/// Return whether a TU-anchor witness carries the path-local evidence required
/// by TUProvableInsertionAnchor.
///
/// Exact-slot witnesses use a stronger, separate ExactSlotBoundary contract and
/// therefore intentionally return false here.
bool tuAnchorWitnessHasProvableEvidence(const TUAnchorWitness &witness);

/// Validate the local obligations for a TU-anchor proof using the caller's
/// already-computed accepted-path inventory.
///
/// The inventory parameter lets RefoldProofLattice preserve its generic path
/// classification semantics while RefoldTUAnchorProof can reuse the same
/// validator with its narrower TU-anchor-only inventory.
ProofDischargeRecord
validateTUAnchorProof(AcceptedPathKind currentPath,
                      const TUAnchorWitness *witness,
                      const AcceptancePathInventory &inventory);

/// Builds accepted-result carriers for TU insertion anchors.
///
/// The class is intentionally narrow: it converts an already-proven
/// TUAnchorWitness into a normalized AcceptedResultCandidate.  The anchor
/// search/proof policy remains in RefoldTUEditPlanner; the lattice remains
/// responsible for ranking and theorem-selection behavior.
class RefoldTUAnchorProof {
public:
  /// \p bToks is forwarded to the shared proof-summary builder, whose only
  /// hard input it is.
  RefoldTUAnchorProof(const RefoldTheoremAudit &theoremAudit,
                      llvm::ArrayRef<PPTok> bToks);

  /// Build the normalized carrier for a proven TU insertion anchor.
  ///
  /// Only TUExactSlotBoundary and TUProvableInsertionAnchor are valid anchor
  /// paths.  Other paths fail closed by producing the same undischarged summary
  /// shape that the lattice-side builder would have produced for a misrouted
  /// TU-anchor request.
  AcceptedResultCandidate
  BuildAcceptedTUAnchorCandidate(AcceptedPathKind currentPath,
                                 const TUAnchorWitness &witness) const;

private:
  const RefoldTheoremAudit &theoremAudit_;
  RefoldProofSummaryBuilder proofSummaryBuilder_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUANCHORPROOF_H

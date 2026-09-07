//===--- RefoldStructuralHunkTilingProof.h ----------------------*- C++ -*-===//
//
// Canonical proof predicates for durable structural hunk tilings.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PROOF_REFOLDSTRUCTURALHUNKTILINGPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PROOF_REFOLDSTRUCTURALHUNKTILINGPROOF_H

#include "proof/RefoldTilingWitnessTypes.h"

namespace clang {
namespace refold {

/// Return whether this nonempty replacement requires the boundary-projection
/// theorem.
///
/// The theorem is mandatory for every partition that preserves preprocessing
/// structure, whether the adjacent token segments have the same or different
/// realizers.  Owner diversity does not authorize an otherwise ambiguous split
/// of the replacement B payload.
bool structuralReplacementRequiresBoundaryProjection(
    const StructuralHunkTilingWitness &witness);

/// Validate the complete preserved-source topology theorem.
///
/// Every `PreservedInPlace` edge must be a zero-token, zero-target carrier for
/// one exact nonempty source interval. Such edges must remain bracketed by the
/// same ordered token-bearing fragments that surrounded them in the original
/// source, carry no reconstructed state transition, and remain disjoint from
/// every token carrier in the durable partition. This predicate validates the
/// persisted witness itself; the final text-edit assembler separately checks
/// the actual normalized byte-edit set, including zero-width insertions.
bool structuralPreservedSourceTopologyIsComplete(
    const StructuralHunkTilingWitness &witness);

/// Validate the complete boundary-projection theorem.
///
/// The durable token segments must exactly and monotonically compose the
/// original A and B envelopes.  Every protected interior A seam must have one
/// ordered lower/upper projection record over all maximum-length local token
/// alignments, satisfying
/// `projectLower(aSplit) == projectUpper(aSplit) == bSplit`, and that B boundary
/// must be the exact common boundary of the neighboring token segments.
bool structuralReplacementBoundaryProjectionIsComplete(
    const StructuralHunkTilingWitness &witness);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PROOF_REFOLDSTRUCTURALHUNKTILINGPROOF_H

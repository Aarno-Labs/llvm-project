//===--- RefoldAnchorWitnessTypes.h -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Anchor and owner-realization witnesses for clang-refold.
//
// Compact evidence records for the three ways an accepted edit can be tied
// back to original source: a TU anchor, an include-preserving anchor, and an
// owner realization.  Each carries the named evidence kind that justified it,
// so a later audit can ask which producer fact discharged the anchor rather
// than re-deriving it from emitted text.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDANCHORWITNESSTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDANCHORWITNESSTYPES_H

#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

using llvm::StringRef;

/// \brief Evidence source used to justify an accepted TU anchor.
///
/// Deterministic TU anchoring rules are represented as explicit proof
/// witnesses so accepted TU-owned insertions can explain which anchor source
/// was used and which non-crossing facts were relied upon.
#define REFOLD_TUANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)                           \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(ExactSlotBoundary)                                                  \
  REFOLD_X(ArgLikeBegin)                                                       \
  REFOLD_X(ImmediateRightNeighbor)                                             \
  REFOLD_X(ImmediateLeftNeighbor)                                              \
  REFOLD_X(IncludeDirectiveBoundary)                                           \
  REFOLD_X(ZeroTokenIncludeBoundary)

enum class TUAnchorEvidenceKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_TUANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(TUAnchorEvidenceKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case TUAnchorEvidenceKind::name:                                             \
    return #name;
    REFOLD_TUANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_TUANCHOR_EVIDENCE_KIND_LIST

/// \brief Compact witness for an accepted TU anchor.
struct TUAnchorWitness {
  TUAnchorEvidenceKind evidence = TUAnchorEvidenceKind::Unknown;
  bool hasPPGap = false;
  uint64_t ppGap = 0;
  bool hasTUByte = false;
  uint64_t tuByte = 0;

  // Exact structural slot anchor metadata.
  bool exactPPMatch = false;
  uint64_t slotId = 0;
  std::string slotKind;

  // Arg-like begin anchor metadata.
  uint64_t macroId = 0;

  // Neighbor-based anchor metadata.
  bool hasLeftNeighbor = false;
  uint64_t leftNeighborPP = 0;
  bool hasRightNeighbor = false;
  uint64_t rightNeighborPP = 0;

  // Ownership/non-crossing metadata for provable TU insertion anchors.
  bool outsideIncludeCoverage = false;
  bool ownerDepthStable = false;
};

/// \brief Evidence source used to justify an accepted include-preserving
/// anchor or mapped include byte range.
///
/// Include-preserving materialization paths use explicit local witnesses so
/// each accepted include patch can explain which deterministic
/// anchoring or mapping rule was used.
#define REFOLD_INCLUDE_ANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)                     \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MappedHeaderTokens)                                                 \
  REFOLD_X(SelectedConditionalBoundary)                                        \
  REFOLD_X(ChildBoundary)                                                      \
  REFOLD_X(RightNeighborPP)                                                    \
  REFOLD_X(LeftNeighborPP)                                                     \
  REFOLD_X(DeclBoundary)

enum class IncludeAnchorEvidenceKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_INCLUDE_ANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(IncludeAnchorEvidenceKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case IncludeAnchorEvidenceKind::name:                                        \
    return #name;
    REFOLD_INCLUDE_ANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_INCLUDE_ANCHOR_EVIDENCE_KIND_LIST

/// \brief Compact witness for an accepted include-preserving path.
struct IncludeAnchorWitness {
  IncludeAnchorEvidenceKind evidence = IncludeAnchorEvidenceKind::Unknown;

  // Concrete byte placement inside the edited header file. Inserts use a
  // single anchor byte; mapped delete/replace paths use an explicit byte
  // range.
  bool hasAnchorByte = false;
  uint64_t anchorByte = 0;
  bool hasByteRange = false;
  uint64_t startByte = 0;
  uint64_t endByte = 0;

  // PP-token provenance for mapped delete/replace paths and neighbor-based
  // insertion anchors.
  bool hasFirstPP = false;
  uint64_t firstPP = 0;
  bool hasLastPP = false;
  uint64_t lastPP = 0;
  bool hasNeighborPP = false;
  uint64_t neighborPP = 0;

  // Conditional, child-include, and declaration boundary metadata.
  bool hasCondArmId = false;
  uint64_t condArmId = 0;
  bool hasChildIncludeId = false;
  uint64_t childIncludeId = 0;
  bool hasDeclHeaderRange = false;
  uint64_t declHeaderB = 0;
  uint64_t declHeaderE = 0;
};

/// \brief Evidence source used to justify an accepted include realization.
///
/// The include-realization domain boundary is explicit. An inline include
/// realization is in-domain only when the include cover admits either the
/// canonical A-cover -> B-envelope mapping or the
/// BoundaryStableConsensusBCoverEnvelope proof.  The latter is not a legacy
/// fallback branch: it is accepted only when all usable non-canonical boundary
/// projections agree on the same non-empty B-token range.  Any include
/// realization outside those declared witnesses remains an explicit terminal
/// out-of-domain case instead of manufacturing a weaker proof class.
#define REFOLD_INCLUDE_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)                \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(CanonicalBCoverEnvelope)                                            \
  REFOLD_X(BoundaryStableConsensusBCoverEnvelope)

enum class IncludeRealizationEvidenceKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_INCLUDE_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(IncludeRealizationEvidenceKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case IncludeRealizationEvidenceKind::name:                                   \
    return #name;
    REFOLD_INCLUDE_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_INCLUDE_REALIZATION_EVIDENCE_KIND_LIST

using IncludeRealizationBTokenEnvelope = std::pair<size_t, size_t>;

/// Owner-polymorphic evidence kind for realized output.
///
/// Macro whole-cover realization, include realization, and direct TU byte
/// realization still use owner-specific spelling mechanics.  This enum gives
/// those paths one shared theorem-facing carrier so the proof lattice can
/// audit all realized output as an OwnerRealizationProof.
#define REFOLD_OWNER_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)                  \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MacroWholeCover)                                                    \
  REFOLD_X(IncludeBEnvelope)                                                   \
  REFOLD_X(IncludeMaterializedExpansion)                                       \
  REFOLD_X(TUByteSpan)                                                         \
  REFOLD_X(TUSpecializedRealization)

enum class OwnerRealizationEvidenceKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_OWNER_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(OwnerRealizationEvidenceKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case OwnerRealizationEvidenceKind::name:                                     \
    return #name;
    REFOLD_OWNER_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_OWNER_REALIZATION_EVIDENCE_KIND_LIST

/// Concrete-carrier proof attached only to ordinary direct TU byte spans.
///
/// This witness records that the direct byte-span theorem was rechecked at the
/// owner-realization boundary using the exact hunk and final `TUByteSpanPlan`.
/// It deliberately does not treat closure widening as source authority. Any
/// protected preprocessing structure was either absent from the emitted span,
/// recognized only as exact macro-transition evidence deferred to the
/// specialized repair planner, or proven to remain outside the edit by a
/// durable structural-segment binding. This witness never grants emission
/// authority to an ordinary direct-TU carrier.
struct TUOwnerRealizationCarrierWitness {
  bool exactHunkAndSpanValidated = false;
  bool protectedStructureExcludedOrDeferred = false;
  bool hasStructuralSegmentBinding = false;
  bool structuralSegmentBindingValidated = false;
  uint64_t structuralWitnessId = 0;
  uint32_t structuralSegmentIndex = 0;

  bool IsComplete() const {
    const bool structuralBindingConsistent =
        hasStructuralSegmentBinding
            ? structuralSegmentBindingValidated && structuralWitnessId != 0
            : !structuralSegmentBindingValidated && structuralWitnessId == 0 &&
                  structuralSegmentIndex == 0;
    return exactHunkAndSpanValidated &&
           protectedStructureExcludedOrDeferred &&
           structuralBindingConsistent;
  }
};

/// Canonical witness for owner realization.
///
/// This does not replace owner-specific emission code.  It records the common
/// proof facts every realized owner must discharge: owner identity, source
/// interval, consumed A-token cover, emitted B-token envelope, and the
/// canonical state summary attached to that owner.  Those checks are
/// centralized in \c TryBuildOwnerRealization(); macro/include/TU callers
/// should only construct the owner-specific closure and spelling, then
/// delegate the shared admissibility proof to that helper.
struct OwnerRealizationWitness {
  OwnerRealizationEvidenceKind evidence = OwnerRealizationEvidenceKind::Unknown;
  OwnerClosure closure;

  // Typed state witnesses for the realized owner. Each witness names the exact
  // state component it discharges, so no coarse enum such as "closure widened"
  // stands in for theorem proof.
  std::vector<SuffixStabilityWitness> stateWitnesses;

  /// Present only for `TUByteSpan` evidence. Specialized TU repair/closure
  /// planners use `TUSpecializedRealization` and may not impersonate this
  /// stronger ordinary-hunk theorem with manufactured token coordinates.
  bool hasTUCarrierWitness = false;
  TUOwnerRealizationCarrierWitness tuCarrierWitness;

  std::string detail;
};

/// Result returned by the shared owner-realization proof helper.
///
/// Owner-specific code should keep constructing replacement text itself, but
/// it should use this result to decide whether the common realization proof
/// was discharged.  A rejected result names the failed strict-domain
/// obligation without immediately changing emission control flow; callers can
/// either stop attaching an OwnerRealizationProof or convert the failure into
/// terminal fallback at their own proof boundary.
struct OwnerRealizationResult {
  bool accepted = false;
  OwnerRealizationWitness witness;
  TerminalFallbackProofFailure failure;
  std::string detail;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDANCHORWITNESSTYPES_H

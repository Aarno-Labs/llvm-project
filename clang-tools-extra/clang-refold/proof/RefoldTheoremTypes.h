//===--- RefoldTheoremTypes.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Theorem-facing proof vocabulary for clang-refold accepted results.
//
// The closed enumerations at the base of the proof lattice: which concrete
// surface an edit is emitted onto, which theorem class justifies it, how it
// realizes the original structure, and how two valid candidates are ranked.
// Everything here is a leaf -- these types name proof concepts and depend on
// no other accepted-result carrier, which is what lets every layer above
// include them without acquiring the rest of the lattice.
//
// This file is the base of the accepted-result carrier stack, which was split
// out of a single 2,400-line header so that a translation unit needing one
// proof concept does not see all of them.  The layering is a strict DAG --
// each header depends only on ones above it -- and every consumer includes
// the layers it directly uses rather than an umbrella over all of them:
//
//   RefoldTheoremTypes         -                     (this file)
//   RefoldAcceptancePathTypes  Theorem
//   RefoldCompletenessTypes    AcceptancePath
//   RefoldAnchorWitnessTypes   -
//   RefoldTilingWitnessTypes   -
//   RefoldProofDischargeTypes  Theorem, AnchorWitness, TilingWitness
//   RefoldCandidateTypes       all six above
//   RefoldMacroPatchTypes      AnchorWitness, Candidate
//
// Each header is self-contained: it compiles alone as a translation unit and
// includes what it directly uses.  Do not rely on a sibling to supply a
// declaration, and do not prune an include merely because the build still
// succeeds without it -- transitive satisfaction through a sibling is the
// hidden dependency this layering exists to prevent.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMTYPES_H

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace clang {
namespace refold {

using llvm::StringRef;

// Pure-insertion provenance carriers used by proof/edit normalization.
// Emission ownership for a pure B-only insertion segment.
enum class BInsertionClaim : uint8_t {
  Unclaimed = 0,
  Standalone = 1 // emitted as a TU/include/arm insertion patch
};

// Provenance record for one token-level pure insertion hunk:
// inserted at A-gap aGap, produced by hunkIndex, spanning B tokens [b0,b1).
struct BInsertionProv {
  uint64_t aGap = 0;      // insertion position in A (pp token gap)
  uint64_t hunkIndex = 0; // index in abTokHunks_
  size_t b0 = 0;          // [b0,b1) in B token space
  size_t b1 = 0;
  BInsertionClaim claim = BInsertionClaim::Unclaimed;
};

/// \brief Closed inventory of concrete emission surfaces.
///
/// This enum intentionally does not replace TheoremProofClass.  It answers
/// only "which concrete artifact surface can reach emission?" so the proof
/// model can force every such surface through AcceptedResultCandidate without
/// rediscovering path names by grep.  Some entries are primary artifact
/// surfaces, while mixed-owner tiling and owner-realization materialization
/// are proof overlays that can coexist with a macro/include/TU primary path.
#define REFOLD_EMISSION_PATH_KIND_LIST(REFOLD_X)                               \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MacroPatch)                                                         \
  REFOLD_X(IncludePatch)                                                       \
  REFOLD_X(TUAnchor)                                                           \
  REFOLD_X(TUTextEdit)                                                         \
  REFOLD_X(TerminalOutOfDomain)                                                \
  REFOLD_X(MixedOwnerTilingSegment)                                            \
  REFOLD_X(OwnerRealizationMaterialization)

enum class EmissionPathKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_EMISSION_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(EmissionPathKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case EmissionPathKind::name:                                                 \
    return #name;
    REFOLD_EMISSION_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_EMISSION_PATH_KIND_LIST

/// \brief Canonical list of emission paths represented by one candidate.
///
/// A normalized candidate always has at most one primary emitted surface
/// (`MacroPatch`, `IncludePatch`, `TUAnchor`, `TUTextEdit`, or
/// `TerminalOutOfDomain`).  It may also carry proof overlays, such as a
/// mixed-owner segment witness or an owner-realization materialization
/// witness.  Keeping those overlays in the same deduplicated inventory avoids
/// another parallel family of booleans while preserving the distinction
/// between construction provenance and theorem proof authority.
struct EmissionPathInventory {
  std::vector<EmissionPathKind> paths;

  bool Contains(EmissionPathKind kind) const {
    for (EmissionPathKind existing : paths)
      if (existing == kind)
        return true;
    return false;
  }

  void Add(EmissionPathKind kind) {
    if (kind == EmissionPathKind::Unknown || Contains(kind))
      return;
    paths.push_back(kind);
  }

  bool empty() const { return paths.empty(); }
};

/// \brief Final theorem-facing proof vocabulary for accepted results.
///
/// This enum is the public proof calculus vocabulary used by comments,
/// theorem-audit logs, and strict-domain tests.  These values answer
/// "what theorem proof discharges this accepted edit?"; they never describe
/// which builder happened to construct the candidate.  Implementation-local
/// classes and paths may continue to exist below, but every emitted candidate
/// must normalize into exactly one of these theorem classes before it can be
/// selected or attached to a byte edit.
#define REFOLD_THEOREM_PROOF_CLASS_LIST(REFOLD_X)                              \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(IdentityPreservingProof)                                            \
  REFOLD_X(InvocationPreservingProof)                                          \
  REFOLD_X(DirectivePreservingProof)                                           \
  REFOLD_X(StateRepairProof)                                                   \
  REFOLD_X(OwnerRealizationProof)                                              \
  REFOLD_X(MixedOwnerTilingProof)                                              \
  REFOLD_X(SuffixStabilizationProof)                                           \
  REFOLD_X(TerminalOutOfDomainProof)

enum class TheoremProofClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_THEOREM_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(TheoremProofClass value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case TheoremProofClass::name:                                                \
    return #name;
    REFOLD_THEOREM_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_THEOREM_PROOF_CLASS_LIST

/// \brief Implementation-local proof family recorded while candidates are
/// built.
///
/// These values are intentionally not the final theorem vocabulary.  They are
/// retained as compact construction metadata so existing builders do not need
/// to be renamed en masse, but selection and emission must use the
/// summary-owned theorem class before treating a candidate as discharged.
#define REFOLD_ACCEPTED_PROOF_CLASS_LIST(REFOLD_X)                             \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(InvocationPreserving)                                               \
  REFOLD_X(InvocationRealization)                                              \
  REFOLD_X(IncludePreserving)                                                  \
  REFOLD_X(IncludeRealization)                                                 \
  REFOLD_X(TUAnchor)                                                           \
  REFOLD_X(TUTextualEdit)                                                      \
  REFOLD_X(TerminalOutOfDomain)

enum class AcceptedProofClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_ACCEPTED_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(AcceptedProofClass value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case AcceptedProofClass::name:                                               \
    return #name;
    REFOLD_ACCEPTED_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_ACCEPTED_PROOF_CLASS_LIST

/// \brief Whether an accepted result preserves original structure or emits
/// a realized edited surface.
#define REFOLD_REALIZATION_MODE_LIST(REFOLD_X)                                 \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(PreserveOriginalStructure)                                          \
  REFOLD_X(RealizeEditedSurface)

enum class RealizationMode : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_REALIZATION_MODE_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(RealizationMode value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case RealizationMode::name:                                                  \
    return #name;
    REFOLD_REALIZATION_MODE_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_REALIZATION_MODE_LIST

/// \brief Ranking bucket for choosing among multiple valid candidates.
///
/// Preference is tracked separately from proof validity so ordering policy
/// remains explicit rather than being hidden in construction order.
#define REFOLD_SELECTION_PREFERENCE_LIST(REFOLD_X)                             \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(PreferStructurePreservation)                                        \
  REFOLD_X(PreferSurfaceRealization)                                           \
  REFOLD_X(PreferExactAnchoring)

enum class SelectionPreference : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_SELECTION_PREFERENCE_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(SelectionPreference value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case SelectionPreference::name:                                              \
    return #name;
    REFOLD_SELECTION_PREFERENCE_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_SELECTION_PREFERENCE_LIST

/// \brief Surface realization choices kept distinct from proof metadata.
#define REFOLD_SURFACE_DISPOSITION_LIST(REFOLD_X)                              \
  REFOLD_X(None)                                                               \
  REFOLD_X(RealizeWholeCoverMacros)                                            \
  REFOLD_X(RealizeInlineTouchedIncludesFromB)                                  \
  REFOLD_X(RealizeMaterializedIncludeExpansion)                                \
  REFOLD_X(RealizeTranslationUnitByteEdit)                                     \
  REFOLD_X(EmitEditedPreprocessedStream)

enum class SurfaceDisposition : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_SURFACE_DISPOSITION_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(SurfaceDisposition value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case SurfaceDisposition::name:                                               \
    return #name;
    REFOLD_SURFACE_DISPOSITION_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "None";
}
#undef REFOLD_SURFACE_DISPOSITION_LIST

/// \brief Named theorem preferences for otherwise local choices.
///
/// Accepted-result ordering stays out of path-specific code.  A builder may
/// attach one of these names only when it has already proved the
/// corresponding witness preconditions, including that the two candidates
/// realize the same edit; a named preference between candidates realizing
/// different edits means nothing.
///
/// These are deliberately *not* part of the proof order.  A named preference
/// is decided by reading a coordinate of the opposing summary that no summary
/// rank consults, so composing it with the ranks yields a relation that is not
/// transitive.  It is consumed pairwise, by the caller that discharged the
/// equivalence obligation, through
/// `RefoldAcceptedResultRanker::ProvenEquivalentArtifactPrefers`.
///
/// Unknown means no named preference applies.
#define REFOLD_THEOREM_SELECTION_TIE_BREAKER_LIST(REFOLD_X)                    \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(ExactTUArgumentEditOverEquivalentMacroArgsOnly)

enum class TheoremSelectionTieBreakerKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_THEOREM_SELECTION_TIE_BREAKER_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(TheoremSelectionTieBreakerKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case TheoremSelectionTieBreakerKind::name:                                   \
    return #name;
    REFOLD_THEOREM_SELECTION_TIE_BREAKER_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_THEOREM_SELECTION_TIE_BREAKER_LIST

/// \brief Implementation path that produced an accepted theorem carrier.
///
/// This inventory is intentionally subordinate to TheoremProofClass.  It may
/// say where a result came from, but the emitted result is justified only by
/// its normalized theorem proof class and discharged obligations.  New paths
/// should be added here only when they also map onto the strict-domain theorem
/// vocabulary.
#define REFOLD_ACCEPTED_PATH_KIND_LIST(REFOLD_X)                               \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MacroArgsOnlyStandard)                                              \
  REFOLD_X(MacroArgsOnlyPasteSingle)                                           \
  REFOLD_X(MacroArgsOnlyPasteMulti)                                            \
  REFOLD_X(MacroArgsOnlyPurePasteOnly)                                         \
  REFOLD_X(MacroArgsOnlyPairedPureInsertion)                                   \
  REFOLD_X(MacroDirectCalleeSubstitution)                                      \
  REFOLD_X(MacroPasteDerivedCalleeSelector)                                    \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplay)                           \
  REFOLD_X(MacroDagSubtreeRoot)                                                \
  REFOLD_X(MacroCallChainSuffix)                                               \
  REFOLD_X(MacroCounterLiteral)                                                \
  REFOLD_X(MacroWholeCoverRealization)                                         \
  REFOLD_X(IncludePatchPendingMaterialization)                                 \
  REFOLD_X(IncludeDeleteReplaceMappedHeaderTokens)                             \
  REFOLD_X(IncludeInsertSelectedConditionalBoundary)                           \
  REFOLD_X(IncludeInsertChildBoundary)                                         \
  REFOLD_X(IncludeInsertRightNeighborPP)                                       \
  REFOLD_X(IncludeInsertLeftNeighborPP)                                        \
  REFOLD_X(IncludeInsertDeclBoundary)                                          \
  REFOLD_X(IncludeRealizationInlineFromB)                                      \
  REFOLD_X(IncludeMaterializedExpansion)                                       \
  REFOLD_X(TUExactSlotBoundary)                                                \
  REFOLD_X(TUProvableInsertionAnchor)                                          \
  REFOLD_X(TUByteSpanMappedEdit)                                               \
  REFOLD_X(TUByteSpanConservativeEdit)                                         \
  REFOLD_X(TUIncludeClosureEdit)                                               \
  REFOLD_X(TerminalEmitEditedPreprocessedStream)

enum class AcceptedPathKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_ACCEPTED_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(AcceptedPathKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case AcceptedPathKind::name:                                                 \
    return #name;
    REFOLD_ACCEPTED_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_ACCEPTED_PATH_KIND_LIST

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTHEOREMTYPES_H

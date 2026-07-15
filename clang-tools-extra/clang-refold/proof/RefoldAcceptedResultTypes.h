//===--- RefoldAcceptedResultTypes.h ---------------------------*- C++ -*-===//
//
// Accepted-result proof lattice carrier records for clang-refold.
//
// These namespace-level proof carriers describe accepted patch candidates,
// theorem summaries, witness metadata, and selection bookkeeping without tying
// the proof vocabulary to RefoldEngine private state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTTYPES_H

#include "core/RefoldModel.h"
#include "line-control/FinalLineControlModel.h"
#include "macro/RefoldMacroStateProof.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "util/StringUtils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

/// \brief Named theorem-lattice tie-breakers for otherwise local choices.
///
/// Accepted-result ordering stays out of path-specific code.  A
/// builder may attach one of these names only when it has already proved the
/// corresponding witness preconditions; the shared lattice selector then owns
/// the actual preference decision.  Unknown means no special tie-breaker.
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

/// \brief Proof-lattice class assigned to an expansion-fallback branch.
///
/// This enum is the inventory for the fallback translation unit.
/// It classifies what an expansion-fallback branch is trying to emit before
/// that branch is allowed to build an accepted-result carrier.  The names are
/// intentionally theorem-facing, not implementation anecdotes: a fallback
/// branch may survive only as an in-domain proof class or as the explicit
/// terminal out-of-domain carrier.
#define REFOLD_EXPANSION_FALLBACK_BRANCH_PROOF_CLASS_LIST(REFOLD_X)            \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(OwnerRealizationProof)                                              \
  REFOLD_X(MixedOwnerTilingProof)                                              \
  REFOLD_X(DirectivePreservingProof)                                           \
  REFOLD_X(TUTextualEditProof)                                                 \
  REFOLD_X(TerminalOutOfDomainProof)

enum class ExpansionFallbackBranchProofClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_EXPANSION_FALLBACK_BRANCH_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(ExpansionFallbackBranchProofClass value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case ExpansionFallbackBranchProofClass::name:                                \
    return #name;
    REFOLD_EXPANSION_FALLBACK_BRANCH_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_EXPANSION_FALLBACK_BRANCH_PROOF_CLASS_LIST

/// \brief Closed inventory of expansion-fallback branches that can emit.
///
/// This enum deliberately enumerates emitting fallback branches separately
/// from their internal rejection checks.  Rejections do not emit; the only
/// emitted surfaces in this inventory are the TU/include source-closure edit
/// and the declared raw-B terminal carrier.  Adding another emitting branch
/// requires adding it here and assigning exactly one proof class below.
#define REFOLD_EXPANSION_FALLBACK_BRANCH_KIND_LIST(REFOLD_X)                   \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(TUIncludeClosureEdit)                                               \
  REFOLD_X(PostStructuralTerminalOutOfDomain)

enum class ExpansionFallbackBranchKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_EXPANSION_FALLBACK_BRANCH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(ExpansionFallbackBranchKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case ExpansionFallbackBranchKind::name:                                      \
    return #name;
    REFOLD_EXPANSION_FALLBACK_BRANCH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_EXPANSION_FALLBACK_BRANCH_KIND_LIST

/// \brief Canonical classification for one fallback branch.
///
/// `branchProofClass` records the fallback-specific proof inventory requested
/// by the current proof model.  `theoremClass` records the current normalized
/// theorem class used by ProofSummary / EmittedProof.  They intentionally
/// differ for TUTextualEditProof because the current theorem lattice
/// represents TU text realizations with the generic OwnerRealizationProof
/// carrier plus an owner realization witness; later proof passes may split that
/// theorem class without changing the branch inventory.
struct ExpansionFallbackBranchClassification {
  ExpansionFallbackBranchKind branch = ExpansionFallbackBranchKind::Unknown;
  ExpansionFallbackBranchProofClass branchProofClass =
      ExpansionFallbackBranchProofClass::Unknown;
  TheoremProofClass theoremClass = TheoremProofClass::Unknown;
  AcceptedPathKind acceptedPath = AcceptedPathKind::Unknown;

  bool IsClassified() const {
    return branch != ExpansionFallbackBranchKind::Unknown &&
           branchProofClass != ExpansionFallbackBranchProofClass::Unknown &&
           theoremClass != TheoremProofClass::Unknown &&
           acceptedPath != AcceptedPathKind::Unknown;
  }
};

inline ExpansionFallbackBranchClassification
ClassifyExpansionFallbackBranch(ExpansionFallbackBranchKind branch) {
  switch (branch) {
  case ExpansionFallbackBranchKind::TUIncludeClosureEdit:
    return {branch, ExpansionFallbackBranchProofClass::TUTextualEditProof,
            TheoremProofClass::OwnerRealizationProof,
            AcceptedPathKind::TUIncludeClosureEdit};
  case ExpansionFallbackBranchKind::PostStructuralTerminalOutOfDomain:
    return {branch, ExpansionFallbackBranchProofClass::TerminalOutOfDomainProof,
            TheoremProofClass::TerminalOutOfDomainProof,
            AcceptedPathKind::TerminalEmitEditedPreprocessedStream};
  case ExpansionFallbackBranchKind::Unknown:
    break;
  }
  return {};
}

/// \brief How directly the current acceptance path is backed by a proof.
#define REFOLD_ACCEPTANCE_SUPPORT_KIND_LIST(REFOLD_X)                          \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(ExplicitProofBacked)                                                \
  REFOLD_X(DeterministicButNotFirstClass)                                      \
  REFOLD_X(ExplicitOutOfDomainClass)

enum class AcceptanceSupportKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_ACCEPTANCE_SUPPORT_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(AcceptanceSupportKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case AcceptanceSupportKind::name:                                            \
    return #name;
    REFOLD_ACCEPTANCE_SUPPORT_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_ACCEPTANCE_SUPPORT_KIND_LIST

/// \brief Future proof-class placeholder targeted by a current path.
#define REFOLD_FUTURE_PROOF_TARGET_LIST(REFOLD_X)                              \
  REFOLD_X(Unknown, "Unknown")                                                 \
  REFOLD_X(MacroStandardArgsOnly, "MacroStandardArgsOnly")                     \
  REFOLD_X(MacroPasteSingle, "MacroPasteSingle")                               \
  REFOLD_X(MacroPasteMultiFixedAnchor, "MacroPasteMultiFixedAnchor")           \
  REFOLD_X(MacroPurePasteOnly, "MacroPurePasteOnly")                           \
  REFOLD_X(MacroPairedPureInsertion, "MacroPairedPureInsertion")               \
  REFOLD_X(MacroPasteDerivedCalleeSelector, "MacroPasteDerivedCalleeSelector") \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplay,                           \
           "MacroRecursiveTupleGeneratedCalleeReplay")                         \
  REFOLD_X(MacroDagLift, "MacroDagLift")                                       \
  REFOLD_X(MacroCallChainSuffixPreservation,                                   \
           "MacroCallChainSuffixPreservation")                                 \
  REFOLD_X(MacroCounterStabilizationRealization,                               \
           "MacroCounterStabilizationRealization")                             \
  REFOLD_X(MacroRealizationWholeCover, "MacroRealizationWholeCover")           \
  REFOLD_X(IncludePatchByMappedHeaderTokens,                                   \
           "IncludePatchByMappedHeaderTokens")                                 \
  REFOLD_X(IncludeConditionalArmCertifiedInsertion,                            \
           "IncludeConditionalArmCertifiedInsertion")                          \
  REFOLD_X(IncludeInsertionByChildBoundary, "IncludeInsertionByChildBoundary") \
  REFOLD_X(IncludeInsertionByRightNeighborPP,                                  \
           "IncludeInsertionByRightNeighborPP")                                \
  REFOLD_X(IncludeInsertionByLeftNeighborPP,                                   \
           "IncludeInsertionByLeftNeighborPP")                                 \
  REFOLD_X(IncludeInsertionByDeclBoundary, "IncludeInsertionByDeclBoundary")   \
  REFOLD_X(IncludeRealizationCover, "IncludeRealizationCover")                 \
  REFOLD_X(IncludeMaterializedExpansionRealization,                            \
           "IncludeMaterializedExpansionRealization")                          \
  REFOLD_X(TUExactSlotAnchor, "TUExactSlotAnchor")                             \
  REFOLD_X(TUProvableInsertionAnchor, "TUProvableInsertionAnchor")             \
  REFOLD_X(TUByteSpanTextualEdit, "TUByteSpanTextualEdit")                     \
  REFOLD_X(TUIncludeClosureEdit, "TUIncludeClosureEdit")                       \
  REFOLD_X(EditedPreprocessedStreamFallback,                                   \
           "ExplicitOutOfDomainTerminalResult")

enum class FutureProofTarget : uint8_t {
#define REFOLD_X(name, text) name,
  REFOLD_FUTURE_PROOF_TARGET_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(FutureProofTarget value) {
  switch (value) {
#define REFOLD_X(name, text)                                                   \
  case FutureProofTarget::name:                                                \
    return text;
    REFOLD_FUTURE_PROOF_TARGET_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_FUTURE_PROOF_TARGET_LIST

/// \brief Inventory record that maps a current acceptance path onto the proof
/// lattice.
struct AcceptancePathInventory {
  AcceptedPathKind currentPath = AcceptedPathKind::Unknown;
  AcceptanceSupportKind support = AcceptanceSupportKind::Unknown;
  FutureProofTarget futureTarget = FutureProofTarget::Unknown;
};

/// \brief Conflict domain used by the global accepted-result lattice.
///
/// This does not change how candidates are chosen. It names the owner
/// domain in which two accepted artifacts may interact so the current global
/// selection and overlap rules can be described explicitly and audited in one
/// place.
#define REFOLD_LATTICE_CONFLICT_DOMAIN_LIST(REFOLD_X)                          \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MacroInvocationRootSpan)                                            \
  REFOLD_X(IncludeOwnerRegion)                                                 \
  REFOLD_X(TUAnchorPoint)                                                      \
  REFOLD_X(WholeTranslationUnit)

enum class LatticeConflictDomain : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_LATTICE_CONFLICT_DOMAIN_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(LatticeConflictDomain value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case LatticeConflictDomain::name:                                            \
    return #name;
    REFOLD_LATTICE_CONFLICT_DOMAIN_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_LATTICE_CONFLICT_DOMAIN_LIST

/// \brief Merge law used when two artifacts in the same lattice domain are
/// compatible.
#define REFOLD_LATTICE_MERGE_LAW_LIST(REFOLD_X)                                \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(DisjointCompose)                                                    \
  REFOLD_X(NestedOuterShadowsInner)                                            \
  REFOLD_X(SelectSingleWitness)                                                \
  REFOLD_X(TerminalReplacesAll)

enum class LatticeMergeLaw : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_LATTICE_MERGE_LAW_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(LatticeMergeLaw value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case LatticeMergeLaw::name:                                                  \
    return #name;
    REFOLD_LATTICE_MERGE_LAW_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_LATTICE_MERGE_LAW_LIST

/// \brief Conflict law used when two artifacts in the same lattice domain
/// are not simultaneously admissible.
#define REFOLD_LATTICE_CONFLICT_LAW_LIST(REFOLD_X)                             \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(RejectPartialOverlap)                                               \
  REFOLD_X(PreferStructurePreservation)                                        \
  REFOLD_X(PreferExactAnchorWitness)                                           \
  REFOLD_X(PreferOwnerPreservingBeforeRealization)                             \
  REFOLD_X(ExplicitOutOfDomainTerminalResult)

enum class LatticeConflictLaw : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_LATTICE_CONFLICT_LAW_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(LatticeConflictLaw value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case LatticeConflictLaw::name:                                               \
    return #name;
    REFOLD_LATTICE_CONFLICT_LAW_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_LATTICE_CONFLICT_LAW_LIST

/// \brief Normalized description of the current global lattice law.
struct GlobalSelectionLattice {
  LatticeConflictDomain domain = LatticeConflictDomain::Unknown;
  LatticeMergeLaw mergeLaw = LatticeMergeLaw::Unknown;
  LatticeConflictLaw conflictLaw = LatticeConflictLaw::Unknown;
};

/// \brief Whether an accepted path currently participates in the declared
/// completeness set.
///
/// This does not claim the engine is globally complete yet. Instead it makes
/// the scope of the completeness claim explicit: accepted paths either
/// already correspond to a declared proof class, remain transitional while a
/// class is still being closed, or sit outside the declared class set
/// entirely (for example an explicit terminal out-of-domain result).
#define REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X)                       \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(DeclaredProofClass)                                                 \
  REFOLD_X(TransitionalGap)                                                    \
  REFOLD_X(ExplicitOutOfDomainClass)

enum class CompletenessCoverageKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(CompletenessCoverageKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case CompletenessCoverageKind::name:                                         \
    return #name;
    REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_COMPLETENESS_COVERAGE_KIND_LIST

/// \brief What completeness promise the engine makes for a covered path.
#define REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X)                    \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MustDiscoverDeclaredOrStrongerCompatible)                           \
  REFOLD_X(NoClaimPendingClassClosure)                                         \
  REFOLD_X(ExplicitlyOutsideDeclaredSet)

enum class CompletenessExpectationKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(CompletenessExpectationKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case CompletenessExpectationKind::name:                                      \
    return #name;
    REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST

/// \brief Normalized completeness contract for the declared domain above.
///
/// This contract answers only one question: whether a theorem-facing carrier
/// already lies in the declared proof-class set, remains an internal-only
/// transitional gap that must not reach emission, or is explicitly outside
/// the declared set as a named terminal boundary.
struct CompletenessContract {
  CompletenessCoverageKind coverage = CompletenessCoverageKind::Unknown;
  CompletenessExpectationKind expectation =
      CompletenessExpectationKind::Unknown;
  FutureProofTarget declaredTarget = FutureProofTarget::Unknown;
  bool countsTowardDeclaredCoverage = false;
  bool hasExplicitExclusion = false;
  TheoremFallbackFailureKind explicitExclusion =
      TheoremFallbackFailureKind::Unknown;
};

/// \brief The summary's position relative to the declared theorem domain.
///
/// This enum is the theorem-domain projection of the same declared-domain
/// statement: theorem-facing results are either in-domain declared proof
/// classes or explicit named out-of-domain classes. Transitional states may
/// still exist internally, but they are not allowed to survive to emission.
#define REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X)                              \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(DeclaredInDomainClass)                                              \
  REFOLD_X(TransitionalGap)                                                    \
  REFOLD_X(ExplicitOutOfDomainClass)

enum class TheoremDomainKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(TheoremDomainKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case TheoremDomainKind::name:                                                \
    return #name;
    REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_THEOREM_DOMAIN_KIND_LIST

/// \brief Explicit theorem-domain contract derived from the same statement.
///
/// The theorem-domain view must say the same thing as the completeness view:
/// theorem-facing carriers are either in-domain declared proof classes or
/// explicit out-of-domain classes. Transitional states may still exist
/// internally, but they must remain non-emitting staging objects.
struct TheoremDomainContract {
  TheoremDomainKind kind = TheoremDomainKind::Unknown;
  bool inDeclaredDomain = false;
  bool countsTowardCompleteness = false;
  FutureProofTarget declaredTarget = FutureProofTarget::Unknown;
  bool hasExplicitExclusion = false;
  TheoremFallbackFailureKind explicitExclusion =
      TheoremFallbackFailureKind::Unknown;
};

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
  REFOLD_X(ZeroTokenIncludeBoundary)                                           \
  REFOLD_X(CorroboratedRightNeighbor)                                          \
  REFOLD_X(CorroboratedLeftNeighbor)

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
  REFOLD_X(TUByteSpan)

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

  // Typed state witnesses for the realized owner.  Each witness names
  // the exact state component it discharges, so owner realization no longer
  // stores a coarse legacy enum such as "closure widened" as theorem proof.
  std::vector<SuffixStabilityWitness> stateWitnesses;

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

/// Edge kind recorded in a persisted mixed-owner tiling witness.
///
/// Token segments are the non-empty A/B envelopes that are emitted as
/// ordinary normalized hunks.  State gaps are zero-token owners that sit
/// between those emitted segments; they do not emit bytes by themselves, but
/// they are part of the proof that the source gap was fully covered and
/// state-composable.
#define REFOLD_MIXED_OWNER_TILING_EDGE_KIND_LIST(REFOLD_X)                     \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(TokenSegment)                                                       \
  REFOLD_X(StateGap)

enum class MixedOwnerTilingEdgeKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_MIXED_OWNER_TILING_EDGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(MixedOwnerTilingEdgeKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case MixedOwnerTilingEdgeKind::name:                                         \
    return #name;
    REFOLD_MIXED_OWNER_TILING_EDGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_MIXED_OWNER_TILING_EDGE_KIND_LIST

/// Durable per-segment proof record for one mixed-owner tiling edge.
///
/// The split itself is theorem-facing instead of treating the
/// partition as a transient normalizer detail.  Every emitted token segment
/// and every zero-token state gap names the parent tiling proof, its stable
/// segment index, the exact A/B token envelope, and the state-transition proof
/// supplied by the owner closure for that segment.  State-gap entries have
/// zero-width A/B envelopes at the adjoining boundary but still carry the
/// source/state closure that made the gap composable.
struct MixedOwnerTilingSegmentWitness {
  uint64_t parentTilingWitnessId = 0;
  uint32_t segmentIndex = 0;
  MixedOwnerTilingEdgeKind kind = MixedOwnerTilingEdgeKind::Unknown;
  uint64_t aStart = 0;
  uint64_t aEnd = 0;
  uint64_t bStart = 0;
  uint64_t bEnd = 0;
  bool zeroTokenStateGap = false;
  bool allowEmptyBEnvelope = false;
  bool ownerClosureComplete = false;
  std::string ownerSignature;
  std::string sourceSignature;
  std::string producerPathSignature;
  std::string targetPPTokenSignature;
  StateTransitionProof ownerTransitionProof;
};

/// Persisted proof for a deterministic mixed-owner tiling.
///
/// The mixed-owner partition is not just a transient normalization step.  The
/// tiling proof is durable: every emitted token segment can point back to the
/// full ordered proof path, including the
/// zero-token state-gap edges that never become token hunks themselves.
struct MixedOwnerTilingWitness {
  uint64_t witnessId = 0;
  uint64_t originalAStart = 0;
  uint64_t originalAEnd = 0;
  uint64_t originalBStart = 0;
  uint64_t originalBEnd = 0;
  uint32_t tokenSegmentCount = 0;
  uint32_t stateGapCount = 0;
  bool stateSummariesComposed = false;
  bool ownerBoundariesComposed = false;
  bool targetTokenStreamComposed = false;
  bool compositionEdgesProven = false;
  std::string globalTargetPPTokenSignature;
  std::string globalCompositionSignature;
  std::vector<MixedOwnerTilingSegmentWitness> segments;
};

/// Reverse index from an emitted token segment back to its mixed-owner
/// tiling.
///
/// The normalizer still emits ordinary token hunks for downstream
/// classifiers. This binding lets those later accepted candidates recover the
/// full tiling witness without changing the hunk type or duplicating
/// state-gap edges in the emitted edit stream.
struct MixedOwnerTilingSegmentBinding {
  uint64_t aStart = 0;
  uint64_t aEnd = 0;
  uint64_t bStart = 0;
  uint64_t bEnd = 0;
  size_t witnessIndex = 0;
  uint64_t parentTilingWitnessId = 0;
  uint32_t segmentIndex = 0;
};

/// \brief Status produced when the engine evaluates a local proof contract.
///
/// The engine records explicit local obligations for every normalized proof
/// summary. Converted selector sites already use the discharge result as the
/// participation gate, while construction paths that still maintain local
/// facts mirror them into the same record so the theorem boundary stays
/// explicit.
#define REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X)                           \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(PendingMaterialization)                                             \
  REFOLD_X(Discharged)                                                         \
  REFOLD_X(Rejected)

enum class ProofDischargeStatus : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(ProofDischargeStatus value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case ProofDischargeStatus::name:                                             \
    return #name;
    REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_PROOF_DISCHARGE_STATUS_LIST

/// \brief Named local obligations used by proof-discharge records.
#define REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X)                            \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(AcceptedPathClassified)                                             \
  REFOLD_X(FutureTargetMapped)                                                 \
  REFOLD_X(ProofRootTracked)                                                   \
  REFOLD_X(MacroProofRootResolved)                                             \
  REFOLD_X(MacroProofRootIsTopLevel)                                           \
  REFOLD_X(MacroPasteWitnessPresent)                                           \
  REFOLD_X(MacroPasteWitnessWellFormed)                                        \
  REFOLD_X(MacroPasteFreeSurfaceTracked)                                       \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplayWitnessTracked)             \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplayPathUnique)                 \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplayTupleUnique)                \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplayReplayUnique)               \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplaySlicesTracked)              \
  REFOLD_X(MacroRecursiveTupleGeneratedCalleeReplaySlicesNonOverlapping)       \
  REFOLD_X(MacroSubtreeCertificateTracked)                                     \
  REFOLD_X(MacroCallChainWitnessTracked)                                       \
  REFOLD_X(CounterStateWitnessTracked)                                         \
  REFOLD_X(SubtreeAdmissibilityTracked)                                        \
  REFOLD_X(WholeCoverBoundsTracked)                                            \
  REFOLD_X(WholeCoverContainmentTracked)                                       \
  REFOLD_X(WholeCoverBoundaryAccountingTracked)                                \
  REFOLD_X(IncludePendingMaterializationClassified)                            \
  REFOLD_X(IncludePatchShapeTracked)                                           \
  REFOLD_X(IncludeAnchorWitnessTracked)                                        \
  REFOLD_X(IncludeAnchorByteTracked)                                           \
  REFOLD_X(IncludeConditionalOwnershipTracked)                                 \
  REFOLD_X(IncludeMappedHeaderRangeTracked)                                    \
  REFOLD_X(IncludeMappedHeaderByteRangeTracked)                                \
  REFOLD_X(IncludeSelectedConditionalBoundaryWitnessTracked)                   \
  REFOLD_X(IncludeChildBoundaryWitnessTracked)                                 \
  REFOLD_X(IncludeRightNeighborWitnessTracked)                                 \
  REFOLD_X(IncludeLeftNeighborWitnessTracked)                                  \
  REFOLD_X(IncludeDeclBoundaryWitnessTracked)                                  \
  REFOLD_X(TUAnchorPathClassified)                                             \
  REFOLD_X(TUAnchorWitnessTracked)                                             \
  REFOLD_X(TUAnchorPPGapTracked)                                               \
  REFOLD_X(TUAnchorByteTracked)                                                \
  REFOLD_X(TUExactSlotWitnessTracked)                                          \
  REFOLD_X(TUProvableEvidenceTracked)                                          \
  REFOLD_X(TUOutsideIncludeCoverageTracked)                                    \
  REFOLD_X(TUOwnerDepthStableTracked)                                          \
  REFOLD_X(ExplicitOutOfDomainResultTracked)                                   \
  REFOLD_X(PrimaryProofClassDeclared)                                          \
  REFOLD_X(OwnerRealizationWitnessTracked)

enum class ProofObligationKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(ProofObligationKind obligation) {
  switch (obligation) {
#define REFOLD_X(name)                                                         \
  case ProofObligationKind::name:                                              \
    return #name;
    REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_PROOF_OBLIGATION_KIND_LIST

/// \brief Why a local proof contract could not be discharged.
#define REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X)                             \
  REFOLD_X(None)                                                               \
  REFOLD_X(PendingMaterialization)                                             \
  REFOLD_X(MissingAcceptedPathClassification)                                  \
  REFOLD_X(MissingFutureTargetMapping)                                         \
  REFOLD_X(MissingProofRoot)                                                   \
  REFOLD_X(MissingMacroProofRootResolution)                                    \
  REFOLD_X(NonTopLevelMacroProofRoot)                                          \
  REFOLD_X(MissingPasteWitness)                                                \
  REFOLD_X(MalformedPasteWitness)                                              \
  REFOLD_X(UnexpectedPasteSurface)                                             \
  REFOLD_X(MissingRecursiveTupleGeneratedCalleeReplayWitness)                  \
  REFOLD_X(NonUniqueRecursiveTupleGeneratedCalleeReplayPath)                   \
  REFOLD_X(NonUniqueRecursiveTupleGeneratedCalleeReplayTuple)                  \
  REFOLD_X(NonUniqueRecursiveTupleGeneratedCalleeReplaySolution)               \
  REFOLD_X(MissingRecursiveTupleGeneratedCalleeReplaySlices)                   \
  REFOLD_X(OverlappingRecursiveTupleGeneratedCalleeReplaySlices)               \
  REFOLD_X(MissingSubtreeCertificate)                                          \
  REFOLD_X(MissingCallChainWitness)                                            \
  REFOLD_X(MissingCounterStateWitness)                                         \
  REFOLD_X(MissingSubtreeAdmissibility)                                        \
  REFOLD_X(MissingWholeCoverBounds)                                            \
  REFOLD_X(MissingWholeCoverContainment)                                       \
  REFOLD_X(MissingWholeCoverBoundaryAccounting)                                \
  REFOLD_X(MissingIncludePatchShape)                                           \
  REFOLD_X(MissingIncludeAnchorWitness)                                        \
  REFOLD_X(MissingIncludeAnchorByte)                                           \
  REFOLD_X(MissingConditionalOwnership)                                        \
  REFOLD_X(MissingMappedHeaderRange)                                           \
  REFOLD_X(MissingMappedHeaderByteRange)                                       \
  REFOLD_X(MissingIncludeSelectedConditionalBoundaryWitness)                   \
  REFOLD_X(MissingIncludeChildBoundaryWitness)                                 \
  REFOLD_X(MissingIncludeRightNeighborWitness)                                 \
  REFOLD_X(MissingIncludeLeftNeighborWitness)                                  \
  REFOLD_X(MissingIncludeDeclBoundaryWitness)                                  \
  REFOLD_X(MissingTUAnchorClassification)                                      \
  REFOLD_X(MissingTUAnchorWitness)                                             \
  REFOLD_X(MissingTUAnchorGap)                                                 \
  REFOLD_X(MissingTUAnchorByte)                                                \
  REFOLD_X(MissingTUExactSlotWitness)                                          \
  REFOLD_X(MissingTUProvableAnchorWitness)                                     \
  REFOLD_X(MissingTUOutsideIncludeCoverageProof)                               \
  REFOLD_X(MissingTUOwnerDepthStability)                                       \
  REFOLD_X(ExplicitOutOfDomainResult)                                          \
  REFOLD_X(MissingPrimaryProofClass)                                           \
  REFOLD_X(MissingOwnerRealizationWitness)

enum class ProofFailureReason : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(ProofFailureReason reason) {
  switch (reason) {
#define REFOLD_X(name)                                                         \
  case ProofFailureReason::name:                                               \
    return #name;
    REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "None";
}
#undef REFOLD_PROOF_FAILURE_REASON_LIST

/// \brief Compact record of class-local obligation discharge.
struct ProofDischargeRecord {
  ProofDischargeStatus status = ProofDischargeStatus::Unknown;
  ProofFailureReason failureReason = ProofFailureReason::None;
  ProofObligationKind failedObligation = ProofObligationKind::Unknown;
  uint16_t obligationsEvaluated = 0;
  uint16_t obligationsSatisfied = 0;
};

/// \brief Accumulates deterministic local proof-obligation discharge state.
///
/// Callers use Require() for ordinary obligations, Fail() for an explicit
/// fail-closed rejection, and Finish() to normalize an otherwise untouched
/// accumulator to a discharged record.  The first failure remains the canonical
/// diagnostic reason so later checks cannot overwrite the original proof gap.
/// The accumulator lives alongside ProofDischargeRecord because its only
/// dependency is that record and the obligation/reason enums above; promoting
/// it to RefoldProofVocabulary.h would create a header cycle.
struct ProofDischargeAccumulator {
  ProofDischargeRecord record;

  explicit ProofDischargeAccumulator(
      ProofDischargeStatus initialStatus = ProofDischargeStatus::Unknown) {
    record.status = initialStatus;
  }

  /// Record a satisfied obligation.
  void Satisfy(ProofObligationKind obligation) {
    (void)obligation;
    ++record.obligationsEvaluated;
    ++record.obligationsSatisfied;
    if (record.status == ProofDischargeStatus::Unknown)
      record.status = ProofDischargeStatus::Discharged;
  }

  /// Record a failed obligation without overwriting the first failure reason.
  void Fail(ProofObligationKind obligation, ProofFailureReason reason) {
    ++record.obligationsEvaluated;
    if (record.failedObligation == ProofObligationKind::Unknown)
      record.failedObligation = obligation;
    if (record.failureReason == ProofFailureReason::None)
      record.failureReason = reason;
    record.status = ProofDischargeStatus::Rejected;
  }

  /// Check one obligation and update the discharge record monotonically.
  void Require(bool condition, ProofObligationKind obligation,
               ProofFailureReason reason) {
    if (condition)
      Satisfy(obligation);
    else
      Fail(obligation, reason);
  }

  /// Return the normalized record, discharging an untouched accumulator.
  ProofDischargeRecord Finish() {
    if (record.status == ProofDischargeStatus::Unknown)
      record.status = ProofDischargeStatus::Discharged;
    return record;
  }
};
struct ProofSummary;
/// \brief Canonical theorem-facing proof carried by an emitted result.
///
/// This is the single object that answers why a selected artifact is
/// theorem-admissible.  Construction provenance such as AcceptedPathKind and
/// AcceptedProofClass may still exist below this layer, but they are not
/// theorem authority: they must first normalize into exactly one
/// `theoremClass` plus the typed witnesses copied here.  The carrier is
/// intentionally value-only and side-effect-free so ProofSummary can own it
/// without changing selector or emission semantics.
struct EmittedProof {
  TheoremProofClass theoremClass = TheoremProofClass::Unknown;
  ProofDischargeRecord discharge;

  std::optional<OwnerRealizationWitness> ownerRealization;
  std::optional<MixedOwnerTilingWitness> mixedOwnerTiling;
  std::optional<TUAnchorWitness> tuAnchor;
  std::optional<IncludeAnchorWitness> includeAnchor;
  std::optional<SuffixStabilityWitness> suffixStability;
  std::optional<TerminalFallbackWitness> terminalFallback;

  bool HasFinalTheoremClass() const {
    return theoremClass != TheoremProofClass::Unknown;
  }
};

/// \brief Common proof-summary carrier used during the proof/lattice model.
///
/// The summary packages the construction inventory, local discharge result,
/// lattice law, completeness contract, explicit theorem-domain position, and
/// canonical emitted proof for one accepted artifact.  Compatibility
/// construction fields remain available to construction sites, but
/// theorem-facing code must consume `emittedProof` rather than re-deriving
/// proof authority from AcceptedProofClass or local side bits.
struct ProofSummary {
  /// Final theorem class declared by the builder after construction
  /// provenance has been normalized.  This field, not AcceptedProofClass, is
  /// the summary-local answer to "why is this valid?".
  TheoremProofClass theoremClass = TheoremProofClass::Unknown;

  AcceptedProofClass acceptedClass = AcceptedProofClass::Unknown;
  RealizationMode realizationMode = RealizationMode::Unknown;
  SelectionPreference preference = SelectionPreference::Unknown;
  SurfaceDisposition surfaceDisposition = SurfaceDisposition::None;
  TheoremSelectionTieBreakerKind selectionTieBreaker =
      TheoremSelectionTieBreakerKind::Unknown;
  AcceptancePathInventory inventory;
  GlobalSelectionLattice lattice;
  CompletenessContract completeness;
  TheoremDomainContract theoremDomain;
  ProofDischargeRecord discharge;
  bool structurePreserving = false;
  uint64_t proofRootMacroId = 0;
  bool hasTUAnchorWitness = false;
  TUAnchorWitness tuAnchorWitness;
  bool hasIncludeAnchorWitness = false;
  IncludeAnchorWitness includeAnchorWitness;
  // shared owner-realization proof carrier.  Realized macro, include, and TU
  // paths now surface through this single theorem-facing witness;
  // owner-specific input metadata remains local to the spelling machinery
  // instead of appearing as separate proof/audit families.
  bool hasOwnerRealizationWitness = false;
  OwnerRealizationWitness ownerRealizationWitness;

  // durable mixed-owner tiling proof.  A candidate that came from a
  // normalized mixed-owner split can carry the whole ordered path here, not
  // merely the individual emitted token segment.
  bool hasMixedOwnerTilingWitness = false;
  MixedOwnerTilingWitness mixedOwnerTilingWitness;

  // State-stabilization witness carried by proof summaries that discharge a
  // suffix/state theorem directly rather than through owner realization.
  // EmittedProof exposes the same witness slot so the remaining MacroPatch
  // mirror fields can be collapsed into one macro-proof carrier.
  bool hasSuffixStabilityWitness = false;
  SuffixStabilityWitness suffixStabilityWitness;

  std::optional<TerminalFallbackWitness> terminalFallbackWitness;

  /// Canonical theorem-facing proof produced by FinalizeProofSummary().
  /// A disengaged optional means the current construction inventory is still
  /// transitional or failed to discharge the theorem obligations.  Keeping
  /// this cached in the summary prevents later selector/emission code from
  /// treating the construction inventory as an independent proof authority.
  std::optional<EmittedProof> emittedProof;

  bool HasCanonicalEmittedProof() const {
    return emittedProof && emittedProof->HasFinalTheoremClass();
  }

  /// True when the summary's primary proof class came from a concrete
  /// accepted-path or patch-proof enum rather than from inferred legacy side
  /// bits. Proof closure requires theorem-facing emitted results to carry
  /// exactly one explicit primary proof class; implicit legacy classification
  /// is therefore allowed only for internal diagnostics and is rejected before
  /// emission.
  bool primaryProofClassExplicit = false;
};

/// \brief Normalized artifact kind carried by an accepted result.
///
/// Accepted macro/include/TU/terminal results are wrapped in one uniform
/// candidate shape before competition. Converted selection sites consume that
/// shape directly; path-specific callers must populate the same proof summary
/// so they can be audited against the global selection law.
#define REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X)                   \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MacroPatch)                                                         \
  REFOLD_X(IncludePatch)                                                       \
  REFOLD_X(TUAnchor)                                                           \
  REFOLD_X(TUTextEdit)                                                         \
  REFOLD_X(TerminalOutOfDomain)

enum class AcceptedResultCandidateKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(AcceptedResultCandidateKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case AcceptedResultCandidateKind::name:                                      \
    return #name;
    REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST

/// \brief Explicit witness state for line-control and builtin-location
/// preservation.
///
/// This carrier moves logical line/file/file-name observer facts into the
/// shared witness vocabulary.  It is deliberately a projection of
/// producer/state-graph facts that already exist on accepted candidates; it
/// does not rescan source text or infer #line semantics from spelling.
struct LineControlObserverWitness {
  bool observesLineNumber = false;
  bool observesFileState = false;
  bool observesFileName = false;

  uint32_t lineControlEventCount = 0;
  uint32_t activeLineControlEventCount = 0;
  uint32_t inactiveLineControlEventCount = 0;
  uint32_t producerProvenLineControlEventCount = 0;
  uint32_t missingOperandLineControlEventCount = 0;
  uint32_t unknownOperandLineControlEventCount = 0;

  uint32_t builtinLocationObservationCount = 0;
  uint32_t builtinLineObservationCount = 0;
  uint32_t builtinFileObservationCount = 0;
  uint32_t builtinFileNameObservationCount = 0;

  uint32_t sourceAuthoredLineDirectiveCount = 0;
  uint32_t producerEmittedLineDirectiveCount = 0;
  uint32_t includeReturnResyncCount = 0;
  uint32_t syntheticResyncCount = 0;

  bool physicalLayoutKnown = false;
  bool physicalLayoutStable = false;
  bool hasSourceLineControlState = false;
  bool hasBuiltinLocationObservers = false;
  bool hasSuffixLineControlDischarge = false;

  std::string stateSignature;
  std::string observerSignature;
  std::string layoutSignature;

  bool Empty() const {
    return !observesLineNumber && !observesFileState && !observesFileName &&
           lineControlEventCount == 0 && builtinLocationObservationCount == 0 &&
           !hasSuffixLineControlDischarge;
  }
};

/// \brief Explicit witness state for `__COUNTER__` preservation.
///
/// Counter sequencing is an ordinary equivalence-key dimension.
/// The witness is a projection of already-proven producer/state facts: it
/// names concrete counter events when available, records whether the accepted
/// repair preserves or materializes the counter suffix state, and keeps the
/// B-side literal value surface separate from ordinary token equivalence.
struct CounterStateWitness {
  bool observesCounter = false;
  bool hasCounterEvents = false;
  bool counterOrderKnown = false;
  bool suffixStateStable = false;
  bool coversAllAffectedObservers = false;
  bool literalizationStable = false;
  bool materializationStable = false;
  bool suffixUnobserved = false;
  bool hasExpectedBValues = false;
  bool hasMissingExpectedBValues = false;

  uint32_t counterConsumptionCount = 0;
  uint32_t counterObservationCount = 0;
  uint32_t counterMutationCount = 0;
  uint32_t preservedSuffixObserverCount = 0;
  uint32_t expectedBValueCount = 0;
  uint32_t missingExpectedBValueCount = 0;

  std::string consumptionSignature;
  std::string orderSignature;
  std::string suffixObserverSignature;
  std::string suffixValueSignature;

  bool Empty() const {
    return !observesCounter && !hasCounterEvents && !suffixStateStable &&
           counterConsumptionCount == 0 && preservedSuffixObserverCount == 0;
  }
};

/// \brief Normalized wrapper for a concrete accepted result.
///
/// The carrier stays intentionally small and explicit. It holds the
/// normalized proof summary plus enough artifact-local provenance for the
/// Patch-B selection sites to compare accepted outcomes through the lattice
/// without rebuilding path-specific ordering logic.
struct AcceptedResultCandidate {
  AcceptedResultCandidateKind kind = AcceptedResultCandidateKind::Unknown;
  ProofSummary proofSummary = {};

  // enumeration of the concrete emission surface(s) represented by this
  // candidate. This is inventory only: proof validity still comes from
  // ProofSummary::emittedProof / TheoremProofClass.
  EmissionPathInventory emissionPaths = {};

  // Artifact-local span / owner metadata.
  uint64_t begin = 0;
  uint64_t end = 0;
  bool hasOwnerIncludeId = false;
  uint64_t ownerIncludeId = 0;
  bool hasRootMacroId = false;
  uint64_t rootMacroId = 0;
  bool hasAnchorByte = false;
  uint64_t anchorByte = 0;

  // Human-readable preview of the selected surface. This is tracing-only and
  // never participates in admissibility or ordering.
  bool hasPayloadPreview = false;
  std::string payloadPreview;

  // witness-equivalence surface for structure-preserving macro actual repair.
  // These fields are copied from MacroPatch only after theexisting macro
  // proof has already validated a whole-envelope replay.  They are proof
  // facts, not selection preferences: the resolver may use them to decide
  // whether two source spellings occupy the same observational class, but the
  // byte spelling itself remains outside the equivalence key.
  bool hasTargetBTokenRange = false;
  uint64_t targetBTokStart = 0;
  uint64_t targetBTokEnd = 0;
  bool hasMacroActualRepairWitness = false;
  bool macroActualWholeEnvelopeReplayValidated = false;
  bool macroActualDefinitionTapeReplayValidated = false;
  bool macroActualArityStable = false;

  // generated-callee / higher-order replay facts.  These are copied from
  // MacroPatchProof only after an existing replay proof has validated the
  // generated callee chain and mapped the solved callee actuals back to the
  // root invocation.  They refine witness equivalence without changing the
  // legacy theorem class or selector ordering.
  bool hasGeneratedCalleeReplayWitness = false;
  uint64_t generatedCalleeRootMacroId = 0;
  uint64_t generatedCalleeFinalDirectiveId = 0;
  uint32_t generatedCalleeDepth = 0;
  uint32_t generatedCalleeObjectAliasHops = 0;
  bool generatedCalleeChainDeterministic = false;
  bool generatedCalleeReplacementReplayValidated = false;
  bool generatedCalleeSolvedActualsMappedToRoot = false;
  bool generatedCalleeUsesForwarding = false;
  bool generatedCalleeUsesStringification = false;
  bool generatedCalleeUsesPaste = false;
  bool generatedCalleeUsesVariadicForwarding = false;
  bool generatedCalleeUsesObjectAlias = false;
  bool generatedCalleeDecodedStringLiteralEvidenceOnly = false;

  // direct stringification proof facts. These fields are populated only from
  // producer-recorded stringify spans on an already accepted
  // invocation-preserving macro patch. They are equivalence-key facts, not
  // replacement text: decoded string-literal payloads are used only to build
  // a canonical comparison signature for the proven stringification edge.
  bool hasStringificationWitness = false;
  uint64_t stringificationRootMacroId = 0;
  uint32_t stringificationSpanCount = 0;
  uint32_t stringificationArgCount = 0;
  bool stringificationWhitespaceNormalized = false;
  bool stringificationEscapedSpellingStable = false;
  std::string stringificationProducerSignature;
  std::string stringificationCanonicalPayloadSignature;

  // token-paste proof facts. These fields summarize producer-recorded
  // paste-token structure and/or an already validated paste replay. They
  // deliberately distinguish left/right/result paste obligations from
  // ordinary adjacent forwarding.
  bool hasTokenPasteWitness = false;
  uint64_t tokenPasteRootMacroId = 0;
  uint32_t tokenPasteSpanCount = 0;
  uint32_t tokenPasteTokenCount = 0;
  uint32_t tokenPastePartCount = 0;
  uint32_t tokenPasteArgPartCount = 0;
  uint32_t tokenPasteLiteralPartCount = 0;
  bool tokenPasteHasLeftProducer = false;
  bool tokenPasteHasRightProducer = false;
  bool tokenPasteResultValidated = false;
  bool tokenPasteDiagnosticSafe = false;
  std::string tokenPasteProducerSignature;
  std::string tokenPasteResultSignature;

  // variadic / comma-elision proof facts. These fields distinguish the
  // source-level pack state from the PP-token effect: missing variadic tails,
  // explicit empty tails, non-empty forwarded packs, VA_OPT comma
  // materialization, GNU comma elision, and literal commas inside the
  // variadic actual are separate equivalence dimensions.
  bool hasVariadicCommaWitness = false;
  uint64_t variadicRootMacroId = 0;
  uint32_t variadicFormalIndex = 0;
  bool variadicArityStable = false;
  bool variadicOriginalMissing = false;
  bool variadicOriginalExplicitEmpty = false;
  bool variadicOriginalNonEmpty = false;
  bool variadicResultMissing = false;
  bool variadicResultExplicitEmpty = false;
  bool variadicResultNonEmpty = false;
  bool variadicLiteralCommaInActual = false;
  bool variadicCommaInserted = false;
  bool variadicCommaDeleted = false;
  bool variadicGnuCommaElision = false;
  bool variadicVaOptPresent = false;
  bool variadicVaOptOriginallyActive = false;
  bool variadicVaOptResultActive = false;
  bool variadicVaOptCommaIntroduced = false;
  bool variadicVaOptCommaDeleted = false;
  uint32_t variadicVaOptNodeCount = 0;
  uint32_t variadicVaOptIncludedCount = 0;
  std::string variadicProducerSignature;
  std::string variadicPackStateSignature;

  // zero-token / boundary-gap proof facts.  These facts are producer-anchored
  // ownership evidence for insertions whose A-side source width is zero:
  // exact TU slots, collapsed include boundaries, empty macro actuals,
  // zero-token replacement-list gaps, and paired pure-insertion frontiers.
  // They deliberately track boundary/layout/observer stability as equivalence
  // dimensions rather than treating all zero-width anchors as
  // interchangeable.
  bool hasZeroTokenBoundaryWitness = false;
  uint64_t zeroTokenOwnerId = 0;
  std::string zeroTokenOwnerKind;
  bool zeroTokenHasPPGap = false;
  uint64_t zeroTokenPPGap = 0;
  bool zeroTokenHasSourceAnchor = false;
  uint64_t zeroTokenSourceAnchor = 0;
  bool zeroTokenHasBTokenRange = false;
  uint64_t zeroTokenBTokStart = 0;
  uint64_t zeroTokenBTokEnd = 0;
  bool zeroTokenProducerProven = false;
  bool zeroTokenOwnerClosed = false;
  bool zeroTokenLayoutStable = false;
  bool zeroTokenObserversStable = false;
  bool zeroTokenCounterStable = false;
  bool zeroTokenFromEmptyActual = false;
  bool zeroTokenFromReplacementGap = false;
  bool zeroTokenFromPairedInsertion = false;
  bool zeroTokenFromTUAnchor = false;
  bool zeroTokenFromIncludeBoundary = false;
  bool zeroTokenFromDirectiveLayoutGap = false;
  std::string zeroTokenBoundarySignature;

  // line-control / builtin-location observer facts.  These fields are
  // attached after the normal proof summary has been built, and are used only
  // by witness tracing / equivalence-key construction.
  bool hasLineControlObserverWitness = false;
  LineControlObserverWitness lineControlObserverWitness;

  // `__COUNTER__` observer/consumption facts.  These fields are projected
  // from typed CounterEventIdentity and SuffixStabilityWitness records.  They
  // are equivalence facts only; they do not decide whether a patch is
  // admissible or alter selector ordering.
  bool hasCounterStateWitness = false;
  CounterStateWitness counterStateWitness;
};

/// \brief Result returned by the accepted-result selector.
///
/// This carrier is theorem-facing: the selected candidate must already
/// normalize to one final emitted proof class. Selector-only staging objects
/// are deliberately kept out of this type so no internal macro-ranking proof
/// can be mistaken for an emitted accepted artifact.
struct SelectedAcceptedResultCandidate {
  AcceptedResultCandidate candidate;
  size_t index = 0;
};

/// \brief Macro-local selector carrier for internal macro competition.
///
/// Macro candidate ranking sometimes needs a staging proof that is valid only
/// for choosing among macro spellings, such as a nested subtree/call-chain
/// candidate whose remaining obligation is the top-level proof-root rule.
/// That selector proof is not an emitted accepted artifact. When the same
/// concrete macro patch also has an emission-normalized proof, the emitted
/// carrier is stored separately and is the only object allowed to be certified
/// onto MacroPatch::selectedAcceptedCandidate.
struct MacroSelectionCandidate {
  AcceptedResultCandidate selectorCandidate;
  std::optional<AcceptedResultCandidate> emittedCandidate;
  bool selectorOnly = false;
};

/// \brief Result returned by the macro-local selector.
///
/// The index points back to the caller-owned MacroPatch entry. The selected
/// macro carrier may have ranked by a selector-only proof, but callers must
/// certify only emittedCandidate, never selectorCandidate, onto an emitted
/// MacroPatch.
struct SelectedMacroSelectionCandidate {
  MacroSelectionCandidate candidate;
  size_t index = 0;
};

#define REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X)                           \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(CounterLiteral)                                                     \
  REFOLD_X(ArgsOnlyPasteMulti)                                                 \
  REFOLD_X(ArgsOnlyPasteSingle)                                                \
  REFOLD_X(ArgsOnlyPurePasteOnly)                                              \
  REFOLD_X(ArgsOnlyStandard)                                                   \
  REFOLD_X(ArgsOnlyPairedPureInsertion)                                        \
  REFOLD_X(PasteDerivedCalleeSelector)                                         \
  REFOLD_X(RecursiveTupleGeneratedCalleeReplay)                                \
  REFOLD_X(DagSubtreeRoot)                                                     \
  REFOLD_X(CallChainSuffix)                                                    \
  REFOLD_X(WholeCoverRealization)

enum class MacroPatchProofKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(MacroPatchProofKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case MacroPatchProofKind::name:                                              \
    return #name;
    REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_MACRO_PATCH_PROOF_KIND_LIST

/// \brief Producer/replay evidence for paste-preserving macro proofs.
///
/// Paste-specific proof evidence lives inside the canonical MacroPatchProof
/// carrier. This witness is the durable home for the paste-specific part of
/// the proof: either the producer supplied well-formed paste-token spans, or
/// the engine replayed the pasted surface and proved that the edited output
/// is exactly reconstructed.
struct PasteWitness {
  uint64_t rootMacroId = 0;
  bool requiresProducerPasteSpans = false;
  bool replayValidated = false;
};

/// \brief Durable certificate for DAG/subtree macro preservation proofs.
///
/// These fields intentionally summarize the existing subtree audit metadata.
/// Centralizing them here lets ClassifyMacroPatchProof() consume one proof
/// object instead of a scattered collection of path-local side bits.
struct SubtreeCertificate {
  bool backed = false;
  uint64_t leafMacroId = 0;
  uint32_t witnessCount = 0;
  uint32_t invocationCertCount = 0;
  uint32_t formalCertCount = 0;
  uint32_t argCertCount = 0;
  uint32_t liftChainCount = 0;
  uint32_t liftStepCount = 0;
  uint32_t rootMergeCount = 0;
  bool usesLexicalBridge = false;
  bool touchesPaste = false;
  bool hasWrapperSemantics = false;
  bool hasStringifySemantics = false;
  bool hasWideStringifySemantics = false;
  bool hasPreferredChildSyntax = false;
  bool hasRawInvocationPreservation = false;
  bool hasPassthroughFlatten = false;
  bool hasBridgeSensitiveStructuredSemantics = false;
  bool deferredPasteDischarged = false;
  bool admissible = false;
  uint32_t expectedRootFormalCount = 0;
  uint32_t deferredRootArgCount = 0;
  uint32_t bridgeSensitiveFormalCount = 0;
  std::string expectedRootFormalSummary;
  std::string deferredRootArgSummary;
  std::string bridgeSensitiveFormalSummary;
};

/// \brief Root/callsite evidence for call-chain suffix preservation.
///
/// A call-chain suffix proof is valid only when the emitted patch is tied to
/// the same root macro invocation that owns the suffix slice. Keeping the
/// relationship in a witness object avoids future proof code having to infer
/// that relationship from unrelated MacroPatch scalar fields.
struct CallChainWitness {
  uint64_t rootMacroId = 0;
  uint64_t callsiteMacroId = 0;
};

/// \brief Producer-path evidence for generated-callee / higher-order replay.
///
/// Generated-callee proofs are not ordinary forwarding even when their final
/// emitted source edit is a root invocation argument rewrite.  The proof
/// follows a deterministic generated-call chain, optionally through
/// object-like aliases, then replays the final callee replacement list as a
/// producer-aware transducer.  Decoded string-literal payloads are recorded
/// as comparison evidence only; they are never treated as replacement text
/// unless the path also carries an explicit stringification producer.
struct GeneratedCalleeReplayWitness {
  uint64_t rootMacroId = 0;
  uint64_t finalDirectiveId = 0;
  uint32_t generatedCallDepth = 0;
  uint32_t objectAliasHops = 0;
  bool calleeChainDeterministic = false;
  bool replacementReplayValidated = false;
  bool solvedActualsMappedToRoot = false;
  bool usesForwarding = false;
  bool usesStringification = false;
  bool usesPaste = false;
  bool usesVariadicForwarding = false;
  bool usesObjectAlias = false;
  bool decodedStringLiteralEvidenceOnly = false;
};

/// \brief One terminal generated-callee actual bound to a root tuple slice.
///
/// The byte offsets are relative to the source-spelled tuple payload, not the
/// complete invocation and not the edited replacement text.  The recursive
/// tuple-generated-callee theorem requires these slices to be exact, ordered,
/// and non-overlapping before any root tuple edit can be emitted.
struct GeneratedActualRootTupleSlice {
  uint32_t generatedActualIndex = 0;
  uint32_t generatedFormalIndex = 0;
  uint32_t rootTupleFormalIndex = 0;
  uint64_t rootTuplePayloadByteBegin = 0;
  uint64_t rootTuplePayloadByteEnd = 0;
};

/// \brief Durable proof carrier for recursive tuple-generated-callee replay.
///
/// This witness is deliberately narrower than ordinary generated-callee replay.
/// It certifies that a root invocation forwards a selector formal and a tuple
/// formal through producer-recorded `caller_macro_id` / `arg_refs` edges until a
/// single terminal generated function-like macro consumes exact elements of
/// that root tuple.  Builders must set the uniqueness and validation bits only
/// after separately proving the named obligation; the classifier rejects the
/// proof kind unless those obligations are all recorded here.
struct RecursiveTupleGeneratedCalleeReplayWitness {
  uint64_t rootInvocationId = 0;
  uint64_t terminalGeneratedInvocationId = 0;
  uint64_t terminalCalleeDefinitionDirectiveId = 0;

  uint32_t rootCalleeFormalIndex = 0;
  uint32_t rootTupleFormalIndex = 0;

  std::vector<GeneratedActualRootTupleSlice> actualSlices;

  bool uniquePath = false;
  bool uniqueTupleFormal = false;
  bool uniqueReplaySolution = false;
};

/// \brief Evidence that an invocation-preserving patch replayed the whole
/// macro expansion envelope, not merely one local formal span.
///
/// Some ArgsOnlyStandard builders solve the complete replacement-list replay
/// problem: literals, formals, repeated occurrences, empty slots, VA_OPT, or
/// higher-order generated callees are checked together against the edited
/// B-side envelope. Those paths may legitimately preserve the source
/// invocation even when a naive literal-body comparison would observe changed
/// downstream tokens. Keep that fact in the canonical proof carrier so the
/// final whole-cover arbitration gate can distinguish complete replay proofs
/// from local current-level formal rewrites.
struct WholeEnvelopeReplayWitness {
  uint64_t rootMacroId = 0;
  bool replayValidated = false;

  // True only for the definition-tape solver, which reparses the recorded
  // replacement list and matches literals, formals, empty variadic slots, and
  // __VA_OPT__ branch choices against the edited B envelope as one complete
  // transducer.  That stronger proof can discharge fixed-body tokens that
  // have no stable A->B token map after a variadic tail becomes empty.
  bool definitionTapeReplayValidated = false;
};

/// \brief Producer-path evidence for variadic / comma-elision replay.
///
/// Variadic macro repairs are not ordinary forwarding. The same final token
/// stream can arise by absorbing text into a fixed formal, by making an
/// omitted pack explicit, by deleting or inserting the source-level separating
/// comma, or by activating/deactivating an `__VA_OPT__` payload.  This witness
/// records those distinctions so the common equivalence key never merges
/// missing, empty, comma-elided, and VA_OPT-mediated repairs by source spelling
/// alone.
struct VariadicCommaWitness {
  uint64_t rootMacroId = 0;
  uint32_t variadicFormalIndex = 0;
  bool arityStable = false;
  bool originalMissing = false;
  bool originalExplicitEmpty = false;
  bool originalNonEmpty = false;
  bool resultMissing = false;
  bool resultExplicitEmpty = false;
  bool resultNonEmpty = false;
  bool literalCommaInActual = false;
  bool commaInserted = false;
  bool commaDeleted = false;
  bool gnuCommaElision = false;
  bool vaOptPresent = false;
  bool vaOptOriginallyActive = false;
  bool vaOptResultActive = false;
  bool vaOptCommaIntroduced = false;
  bool vaOptCommaDeleted = false;
  uint32_t vaOptNodeCount = 0;
  uint32_t vaOptIncludedCount = 0;
  std::string producerSignature;
  std::string packStateSignature;
};

/// \brief Producer-path evidence for zero-token insertion and boundary-gap
/// repairs.
///
/// A zero-token repair is not justified by nearest-token guessing.  The
/// anchor must name a producer-proven owner boundary, and the witness records
/// whether layout, preserved observers, and counter state are known stable.
/// Different zero-width anchors remain non-equivalent unless these semantic
/// dimensions match.
struct ZeroTokenBoundaryWitness {
  uint64_t ownerId = 0;
  std::string ownerKind;
  bool hasPPGap = false;
  uint64_t ppGap = 0;
  bool hasSourceAnchor = false;
  uint64_t sourceAnchor = 0;
  bool hasBTokenRange = false;
  uint64_t bTokStart = 0;
  uint64_t bTokEnd = 0;
  bool producerProven = false;
  bool ownerClosed = false;
  bool layoutStable = false;
  bool observersStable = false;
  bool counterStable = false;
  bool fromEmptyActual = false;
  bool fromReplacementGap = false;
  bool fromPairedInsertion = false;
  bool fromTUAnchor = false;
  bool fromIncludeBoundary = false;
  bool fromDirectiveLayoutGap = false;
  std::string boundarySignature;
};

/// \brief Canonical MacroPatch-local proof carrier.
///
/// This object is the only MacroPatch-local proof authority.
/// SetMacroPatchProof() installs it, SyncMacroPatchProofSummary() refreshes
/// derived paste/subtree/call-chain witnesses, and ClassifyMacroPatchProof()
/// copies the resulting theorem facts into ProofSummary / EmittedProof.
struct MacroPatchProof {
  MacroPatchProofKind kind = MacroPatchProofKind::Unknown;
  uint64_t proofRootMacroId = 0;
  bool preservesInvocationStructure = false;

  std::optional<OwnerRealizationWitness> ownerRealization;
  std::optional<SuffixStabilityWitness> suffixStability;
  std::optional<PasteWitness> paste;
  std::optional<SubtreeCertificate> subtree;
  std::optional<CallChainWitness> callChain;
  std::optional<GeneratedCalleeReplayWitness> generatedCalleeReplay;
  std::optional<RecursiveTupleGeneratedCalleeReplayWitness>
      recursiveTupleGeneratedCalleeReplay;
  std::optional<WholeEnvelopeReplayWitness> wholeEnvelopeReplay;
  std::optional<VariadicCommaWitness> variadicCommaReplay;
  std::optional<ZeroTokenBoundaryWitness> zeroTokenBoundaryReplay;
  std::optional<CounterStateWitness> counterState;
};

struct WholeCoverPlan {
  uint64_t covLoA = 0;
  uint64_t covHiA = 0;
  bool usedBodyRange = false;
  bool selfContained = false;
  size_t rawBTokStart = 0;
  size_t rawBTokEnd = 0;
  size_t bTokStart = 0;
  size_t bTokEnd = 0;
  bool adjustedLeft = false;
  bool adjustedRight = false;
  bool claimsClipped = false;
  std::string clippedText;
};

enum class OccurrenceSupportMode {
  /// Only metadata attached directly to the current invocation may justify
  /// occurrence-consistency success. Use this for direct args-only patch
  /// construction so descendant/sibling support cannot silently substitute
  /// for missing current-invocation evidence.
  CurrentInvocationOnly,

  /// Descendant/sibling dependency paths may justify occurrence-consistency
  /// success. Use this only inside the semantic/DAG certificate pipeline,
  /// where the caller is already proving carried rewrites through the
  /// invocation graph.
  AllowGraphSupport,
};

// Counter-stabilization carrier records used by proof lattice APIs.
struct ForcedMacroPatchRequest {
  const RefoldModel::MacroInvocation *macro = nullptr;
  uint64_t aStart = 0;
  uint64_t aEnd = 0;
  CounterEventIdentity event;
};

struct CounterOccurrence {
  const RefoldModel::MacroInvocation *macro;
  uint64_t aStart;
  uint64_t aEnd;
  std::optional<uint64_t> ownerIncludeId;
  CounterEventIdentity event;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTTYPES_H

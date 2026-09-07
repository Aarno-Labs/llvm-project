//===--- RefoldAcceptancePathTypes.h ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Acceptance-path construction inventory for clang-refold.
//
// Records how a candidate was *built* -- which implementation path produced
// it, how directly a proof backs that path, and which future proof class the
// path is targeted at.  This is construction provenance, never proof
// authority: `ProofSummary::theoremClass` answers \"why is this valid?\", and
// nothing here may be read as an answer to that question.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTANCEPATHTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTANCEPATHTYPES_H

#include "proof/RefoldTheoremTypes.h"

#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace clang {
namespace refold {

using llvm::StringRef;

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
  REFOLD_X(MacroDirectCalleeSubstitution, "MacroDirectCalleeSubstitution")     \
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

/// \brief Construction inventory for one accepted result: which acceptance
/// path built it, what kind of support that path claims, and which proof
/// class it is being migrated toward.
///
/// This is provenance, not authority.  Selection and emission consume
/// `ProofSummary::theoremClass` and the canonical `EmittedProof`; the fields
/// here only record how the carrier was constructed.
struct AcceptancePathInventory {
  AcceptedPathKind currentPath = AcceptedPathKind::Unknown;
  AcceptanceSupportKind support = AcceptanceSupportKind::Unknown;
  FutureProofTarget futureTarget = FutureProofTarget::Unknown;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTANCEPATHTYPES_H

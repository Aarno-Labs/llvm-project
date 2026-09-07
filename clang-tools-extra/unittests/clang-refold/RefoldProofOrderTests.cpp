//===-- RefoldProofOrderTests.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the order laws that representative selection depends on.
//
// `SelectPreferredCandidateIndex` picks a winner with a single left-to-right
// max scan: it keeps one running best and never reconsiders a candidate it has
// discarded.  That returns the same winner for every candidate push order only
// when the relation it scans is a strict order.  When it is not, the same set
// of proofs selects a different artifact depending on the order a caller
// happened to push them in, and nothing downstream can tell that happened.
//
// Two coordinates used to break that precondition, and neither is reachable
// from a lit test:
//
//   * The mixed-owner tiling rule applied only when the two summaries also
//     agreed on `inventory.currentPath`.  Reaching a disagreement needs a
//     `MixedOwnerTilingProof` summary competing against a differently-pathed
//     one inside a single candidate vector.  Instrumenting every consult of
//     the rule across the suite records 68 of them in 25 files, and the paths
//     agree in all 68, so the guard never discriminates there.
//   * `TheoremSelectionTieBreakerKind` is assigned at exactly one site, on a
//     local candidate consumed pairwise and then discarded.  The same
//     instrumentation records zero summaries carrying a non-Unknown
//     tie-breaker reaching the comparator at all.
//
// So the properties are pinned here, where the offending triples can be
// constructed directly from `ProofSummary` values with no producer artifacts:
// the order laws hold on the shapes that used to break them, the named
// tie-breaker still decides the one competition it exists for, and the audit
// that now guards every selection really does reject a cyclic relation.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldAcceptedResultRanker.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/ArrayRef.h"

#include "gtest/gtest.h"

#include <cstddef>
#include <optional>
#include <vector>

using namespace clang::refold;

namespace {

/// Base summary for the order triples: everything that would short-circuit the
/// comparison before the mixed-owner rule is left at its default, so the two
/// rank comparisons tie and control reaches the coordinates under test.
ProofSummary ownerRealizationSummary(AcceptedPathKind path) {
  ProofSummary summary;
  summary.theoremClass = TheoremProofClass::OwnerRealizationProof;
  summary.primaryProofClassExplicit = true;
  summary.inventory.currentPath = path;
  return summary;
}

/// The same summary re-proved by a durable mixed-owner tiling.  This is the
/// shape `AttachMixedOwnerTilingWitnessForTokenEnvelope` builds: the owner
/// summary with a tiling witness attached and the theorem class raised.
ProofSummary mixedOwnerTilingSummary(AcceptedPathKind path) {
  ProofSummary summary = ownerRealizationSummary(path);
  summary.theoremClass = TheoremProofClass::MixedOwnerTilingProof;
  summary.hasMixedOwnerTilingWitness = true;
  summary.mixedOwnerTilingWitness.witnessId = 1;
  summary.mixedOwnerTilingWitness.originalAStart = 0;
  summary.mixedOwnerTilingWitness.originalAEnd = 4;
  summary.mixedOwnerTilingWitness.originalBStart = 0;
  summary.mixedOwnerTilingWitness.originalBEnd = 4;
  summary.mixedOwnerTilingWitness.tokenSegmentCount = 2;
  summary.mixedOwnerTilingWitness.stateGapCount = 1;
  return summary;
}

/// Audit `ProofDominates` over a concrete set of summaries, addressing them by
/// position the way the selector addresses candidates.
std::optional<SelectionOrderViolation>
findProofOrderViolation(llvm::ArrayRef<ProofSummary> summaries) {
  std::vector<size_t> indices;
  for (size_t i = 0; i < summaries.size(); ++i)
    indices.push_back(i);
  return RefoldAcceptedResultRanker::FindSelectionOrderViolation(
      indices, [&](size_t lhs, size_t rhs) {
        return RefoldAcceptedResultRanker::ProofDominates(summaries[lhs],
                                                          summaries[rhs]);
      });
}

//===----------------------------------------------------------------------===//
// The audit itself.
//===----------------------------------------------------------------------===//

TEST(RefoldProofOrder, AuditAcceptsAStrictOrder) {
  // A total order by index: 0 beats 1 beats 2, and 0 beats 2.
  const std::vector<size_t> indices = {0, 1, 2};
  EXPECT_FALSE(RefoldAcceptedResultRanker::FindSelectionOrderViolation(
                   indices, [](size_t lhs, size_t rhs) { return lhs < rhs; })
                   .has_value());
}

TEST(RefoldProofOrder, AuditRejectsAnIrreflexiveViolation) {
  const std::vector<size_t> indices = {0, 1};
  std::optional<SelectionOrderViolation> violation =
      RefoldAcceptedResultRanker::FindSelectionOrderViolation(
          indices, [](size_t lhs, size_t rhs) { return lhs <= rhs; });
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->law, SelectionOrderLaw::Irreflexivity);
  EXPECT_EQ(violation->first, 0u);
}

TEST(RefoldProofOrder, AuditRejectsAnAsymmetryViolation) {
  const std::vector<size_t> indices = {0, 1};
  std::optional<SelectionOrderViolation> violation =
      RefoldAcceptedResultRanker::FindSelectionOrderViolation(
          indices, [](size_t lhs, size_t rhs) { return lhs != rhs; });
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->law, SelectionOrderLaw::Asymmetry);
  EXPECT_EQ(violation->first, 0u);
  EXPECT_EQ(violation->second, 1u);
}

TEST(RefoldProofOrder, AuditRejectsTheCycleAMaxScanCannotResolve) {
  // The defect stated directly: 0 beats 1 beats 2 beats 0.  A left-to-right
  // max scan over [0,1,2] keeps 2; over [2,1,0] it keeps 0.  Same three
  // proofs, different answer, decided by push order.
  const std::vector<size_t> indices = {0, 1, 2};
  auto cyclic = [](size_t lhs, size_t rhs) { return (lhs + 1) % 3 == rhs; };

  std::optional<SelectionOrderViolation> violation =
      RefoldAcceptedResultRanker::FindSelectionOrderViolation(indices, cyclic);
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->law, SelectionOrderLaw::Transitivity);
  EXPECT_EQ(violation->first, 0u);
  EXPECT_EQ(violation->second, 1u);
  EXPECT_EQ(violation->third, 2u);
}

//===----------------------------------------------------------------------===//
// The proof order over real summaries.
//===----------------------------------------------------------------------===//

TEST(RefoldProofOrder, MixedOwnerTilingDominanceDoesNotDependOnThePath) {
  // A durable mixed-owner tiling is either a stronger proof than the
  // owner-specific summary it competes with or it is not.  Making the answer
  // depend on whether a third coordinate matches is what cost transitivity.
  const ProofSummary mixed =
      mixedOwnerTilingSummary(AcceptedPathKind::MacroArgsOnlyStandard);
  const ProofSummary samePath =
      ownerRealizationSummary(AcceptedPathKind::MacroArgsOnlyStandard);
  const ProofSummary otherPath =
      ownerRealizationSummary(AcceptedPathKind::MacroWholeCoverRealization);

  EXPECT_TRUE(RefoldAcceptedResultRanker::ProofDominates(mixed, samePath));
  EXPECT_TRUE(RefoldAcceptedResultRanker::ProofDominates(mixed, otherPath));
  EXPECT_FALSE(RefoldAcceptedResultRanker::ProofDominates(samePath, mixed));
  EXPECT_FALSE(RefoldAcceptedResultRanker::ProofDominates(otherPath, mixed));
}

TEST(RefoldProofOrder, MixedOwnerVersusWholeCoverTripleIsAcyclic) {
  // The exact triple that used to cycle.  With the path guard in place:
  // A beat B because the paths matched and A was the mixed-owner proof; B beat
  // C because the theorem classes tied at OwnerRealizationProof and the path
  // enum fell 1 < 12; and C beat A because the differing paths skipped the
  // mixed-owner rule and OwnerRealizationProof(5) < MixedOwnerTilingProof(6).
  const std::vector<ProofSummary> triple = {
      mixedOwnerTilingSummary(AcceptedPathKind::MacroArgsOnlyStandard),
      ownerRealizationSummary(AcceptedPathKind::MacroArgsOnlyStandard),
      ownerRealizationSummary(AcceptedPathKind::MacroWholeCoverRealization),
  };

  EXPECT_FALSE(findProofOrderViolation(triple).has_value());

  // And the cycle's third edge is the one that is now gone: the mixed-owner
  // proof is no longer beaten by a differently-pathed owner realization.
  EXPECT_FALSE(
      RefoldAcceptedResultRanker::ProofDominates(triple[2], triple[0]));
}

TEST(RefoldProofOrder, ProofOrderIsIrreflexive) {
  const ProofSummary mixed =
      mixedOwnerTilingSummary(AcceptedPathKind::MacroArgsOnlyStandard);
  const ProofSummary owner =
      ownerRealizationSummary(AcceptedPathKind::TUByteSpanMappedEdit);

  EXPECT_FALSE(RefoldAcceptedResultRanker::ProofDominates(mixed, mixed));
  EXPECT_FALSE(RefoldAcceptedResultRanker::ProofDominates(owner, owner));
}

//===----------------------------------------------------------------------===//
// The named theorem tie-breaker, which is deliberately outside the order.
//===----------------------------------------------------------------------===//

/// The two competitors of the one named tie-breaker: an exact TU byte edit
/// inside the original argument spelling, and the args-only macro rewrite of
/// the same invocation.  The caller that builds these has already proved they
/// realize the same edit; that proof is not in either summary.
ProofSummary exactTUArgumentEditSummary() {
  ProofSummary summary =
      ownerRealizationSummary(AcceptedPathKind::TUByteSpanMappedEdit);
  summary.preference = SelectionPreference::PreferSurfaceRealization;
  summary.selectionTieBreaker = TheoremSelectionTieBreakerKind::
      ExactTUArgumentEditOverEquivalentMacroArgsOnly;
  return summary;
}

ProofSummary equivalentMacroArgsOnlySummary() {
  ProofSummary summary =
      ownerRealizationSummary(AcceptedPathKind::MacroArgsOnlyStandard);
  summary.preference = SelectionPreference::PreferStructurePreservation;
  return summary;
}

TEST(RefoldProofOrder, NamedTieBreakerIsNotVisibleToTheProofOrder) {
  const ProofSummary tuEdit = exactTUArgumentEditSummary();
  const ProofSummary macroArgsOnly = equivalentMacroArgsOnlySummary();

  // On summary evidence alone the macro rewrite wins: it is the
  // structure-preserving proof.  The order says so in both directions, and it
  // says so without consulting the tie-breaker, which is the point -- a
  // preference conditioned on a fact the summaries do not carry cannot be
  // part of a relation over summaries.
  EXPECT_FALSE(
      RefoldAcceptedResultRanker::ProofDominates(tuEdit, macroArgsOnly));
  EXPECT_TRUE(
      RefoldAcceptedResultRanker::ProofDominates(macroArgsOnly, tuEdit));
}

TEST(RefoldProofOrder, NamedTieBreakerDecidesTheProvenEquivalentCompetition) {
  const ProofSummary tuEdit = exactTUArgumentEditSummary();
  const ProofSummary macroArgsOnly = equivalentMacroArgsOnlySummary();

  // A caller holding the equivalence proof gets the named preference, which
  // overrides the order it would otherwise lose to.  This is the behavior the
  // one assignment site depends on, and no lit test reaches it.
  ASSERT_TRUE(RefoldAcceptedResultRanker::NamedTheoremTieBreakerPrefers(
                  tuEdit, macroArgsOnly)
                  .has_value());
  EXPECT_TRUE(*RefoldAcceptedResultRanker::NamedTheoremTieBreakerPrefers(
      tuEdit, macroArgsOnly));
  EXPECT_TRUE(RefoldAcceptedResultRanker::ProvenEquivalentArtifactPrefers(
      tuEdit, macroArgsOnly));
  EXPECT_FALSE(RefoldAcceptedResultRanker::ProvenEquivalentArtifactPrefers(
      macroArgsOnly, tuEdit));
}

TEST(RefoldProofOrder, MutualNamedTieBreakerOrdersNothing) {
  // Both sides naming a preference against the other decides nothing, and
  // answering either way would make the composite claim both that each side
  // wins.  Falling through to the order keeps the pairwise consumption honest.
  ProofSummary first = exactTUArgumentEditSummary();
  first.inventory.currentPath = AcceptedPathKind::MacroArgsOnlyStandard;
  ProofSummary second = first;

  EXPECT_FALSE(
      RefoldAcceptedResultRanker::NamedTheoremTieBreakerPrefers(first, second)
          .has_value());
  EXPECT_FALSE(RefoldAcceptedResultRanker::ProvenEquivalentArtifactPrefers(
      first, second));
  EXPECT_FALSE(RefoldAcceptedResultRanker::ProvenEquivalentArtifactPrefers(
      second, first));
}

//===----------------------------------------------------------------------===//
// Incomparability is an answer, not a missing preference.
//===----------------------------------------------------------------------===//

TEST(RefoldProofOrder, IncomparableSummariesAreReportedAsSuch) {
  AcceptedResultCandidate lhs;
  AcceptedResultCandidate rhs;
  lhs.proofSummary =
      ownerRealizationSummary(AcceptedPathKind::TUByteSpanMappedEdit);
  rhs.proofSummary = lhs.proofSummary;

  EXPECT_EQ(RefoldAcceptedResultRanker::CompareAcceptedResultCandidateProofs(
                lhs, rhs),
            ProofDominanceOrder::Incomparable);

  rhs.proofSummary =
      mixedOwnerTilingSummary(AcceptedPathKind::TUByteSpanMappedEdit);
  EXPECT_EQ(RefoldAcceptedResultRanker::CompareAcceptedResultCandidateProofs(
                lhs, rhs),
            ProofDominanceOrder::RightDominates);
  EXPECT_EQ(RefoldAcceptedResultRanker::CompareAcceptedResultCandidateProofs(
                rhs, lhs),
            ProofDominanceOrder::LeftDominates);
}

} // namespace

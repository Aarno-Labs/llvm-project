//===-- RefoldAlignmentSemanticLedgerTests.cpp ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for `findAlignmentSemanticLedgerDefect`, the audit that makes each
// semantic alignment witness prove exactly the theorem it names.
//
// Witnesses once recorded every committed window as a complete equivalence
// over all optimal maps, whichever rule committed it.  The audit now derives
// each resolution kind's candidate domain, key kind, repair scope and anchor
// bases independently of the resolver, and checks the witness against the
// certification window it names and the structure-preserving tie ranges
// production realizes.
//
// The resolver never produces a contradictory witness, so no lit test reaches
// a refusal here: across the suite every production ledger audits clean.  Each
// contradiction is pinned below instead, against one well-formed ledger that
// holds one witness of every kind, and each test asserts the specific defect so
// that removing the check it names fails that test rather than being masked by
// a neighbouring one.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldAlignmentSemanticResolver.h"

#include "gtest/gtest.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace clang::refold;

namespace {

/// Twelve A tokens aligned one-to-one with twelve B tokens, split into three
/// certified windows of four.  The first token of each window is core-forced;
/// the other three are proved by the window's witness: a complete-ground-set
/// equivalence, a required-anchor carrier equivalence, and a
/// structure-respecting recovery, in that order.
struct WellFormedLedger {
  std::vector<AlignmentSemanticResolutionWitness> witnesses;
  std::vector<int64_t> selectedMap;
  std::vector<diffutils::LcsAnchorProof> anchorProofs;
  std::vector<diffutils::LcsCertificationWindow> windows;
  std::vector<std::pair<uint64_t, uint64_t>> tieRanges = {{8, 12}};

  WellFormedLedger() {
    for (int64_t token = 0; token < 12; ++token)
      selectedMap.push_back(token);
    for (uint64_t index = 0; index < 3; ++index) {
      diffutils::LcsCertificationWindow window;
      window.aBegin = window.bBegin = 4 * index;
      window.aEnd = window.bEnd = 4 * index + 4;
      window.status = diffutils::LcsWindowCertificationStatus::Certified;
      windows.push_back(window);
    }

    witnesses.push_back(makeWitness(
        1, 0, AlignmentSemanticResolutionKind::CompleteGroundSetEquivalence,
        AlignmentSemanticCandidateDomain::AllOptimalMaps,
        AlignmentSemanticEquivalenceKeyKind::ConcreteOutput,
        AlignmentSemanticAnchorBasis::EquivalentRealizationRepresentative));
    witnesses.push_back(makeWitness(
        2, 1, AlignmentSemanticResolutionKind::RequiredAnchorCarrierEquivalence,
        AlignmentSemanticCandidateDomain::RequiredAnchorCarriers,
        AlignmentSemanticEquivalenceKeyKind::Realization,
        AlignmentSemanticAnchorBasis::EquivalentRealizationRepresentative));
    witnesses[1].anchorEvidence.front().basis =
        AlignmentSemanticAnchorBasis::FinalSourceNecessary;
    witnesses.push_back(makeWitness(
        3, 2,
        AlignmentSemanticResolutionKind::StructureRespectingTerminalRecovery,
        AlignmentSemanticCandidateDomain::StructureRepairCandidates,
        AlignmentSemanticEquivalenceKeyKind::ConcreteOutput,
        AlignmentSemanticAnchorBasis::
            StructureRespectingTerminalRecoveryRepresentative));
    witnesses[2].structureRepairScope =
        StructureRepairScope::AllPreferredSubRectangles;
    witnesses[2].variedSubRectangleCount = 1;

    anchorProofs.resize(selectedMap.size());
    for (size_t token = 0; token < selectedMap.size(); ++token) {
      if (token % 4 == 0) {
        anchorProofs[token].kind =
            diffutils::LcsAnchorProofKind::CoreOptimalPathForced;
        continue;
      }
      anchorProofs[token].kind =
          diffutils::LcsAnchorProofKind::SemanticResolutionWitness;
      anchorProofs[token].semanticWitnessId = token / 4 + 1;
    }
  }

  /// A witness for window \p windowIndex whose evidence covers its three
  /// non-forced tokens under \p basis.
  AlignmentSemanticResolutionWitness
  makeWitness(uint64_t witnessId, size_t windowIndex,
              AlignmentSemanticResolutionKind kind,
              AlignmentSemanticCandidateDomain domain,
              AlignmentSemanticEquivalenceKeyKind keyKind,
              AlignmentSemanticAnchorBasis basis) const {
    AlignmentSemanticResolutionWitness witness;
    witness.witnessId = witnessId;
    witness.windowIndex = windowIndex;
    witness.aBegin = witness.bBegin = windows[windowIndex].aBegin;
    witness.aEnd = witness.bEnd = windows[windowIndex].aEnd;
    witness.resolutionKind = kind;
    witness.candidateDomain = domain;
    witness.candidateDomainCompletelyEnumerated = true;
    witness.candidateCount = 3;
    witness.acceptedCandidateCount = 1;
    witness.rejectedCandidateCount = 2;
    witness.equivalenceKeyKind = keyKind;
    witness.equivalenceKey = "key";
    witness.representativeMap = selectedMap;
    for (uint64_t token = witness.aBegin + 1; token < witness.aEnd; ++token)
      witness.anchorEvidence.push_back({token, token, basis});
    return witness;
  }

  std::optional<std::string> audit() const {
    return findAlignmentSemanticLedgerDefect(AlignmentSemanticLedger{
        witnesses, selectedMap, anchorProofs, windows, tieRanges,
        /*theoremActive=*/true});
  }
};

/// Expect \p defect to be present and to mention \p fragment.
void expectDefect(const std::optional<std::string> &defect,
                  llvm::StringRef fragment) {
  ASSERT_TRUE(defect.has_value());
  EXPECT_NE(defect->find(fragment.str()), std::string::npos) << *defect;
}

constexpr llvm::StringRef BasisDefect =
    "under a basis its theorem does not establish";

/// The well-formed ledger with windows 1 and 2 committed by one joint
/// structure-respecting recovery instead: both witnesses record the scope
/// `JointAcrossWindows`, the window set {1, 2}, one candidate census and one
/// key, and each owns the tie range over its own window.
WellFormedLedger jointLedger() {
  WellFormedLedger ledger;
  ledger.witnesses[1] = ledger.makeWitness(
      2, 1,
      AlignmentSemanticResolutionKind::StructureRespectingTerminalRecovery,
      AlignmentSemanticCandidateDomain::StructureRepairCandidates,
      AlignmentSemanticEquivalenceKeyKind::ConcreteOutput,
      AlignmentSemanticAnchorBasis::
          StructureRespectingTerminalRecoveryRepresentative);
  for (size_t index : {1, 2}) {
    AlignmentSemanticResolutionWitness &witness = ledger.witnesses[index];
    witness.structureRepairScope = StructureRepairScope::JointAcrossWindows;
    witness.variedSubRectangleCount = 1;
    witness.jointWindowIndices = {1, 2};
  }
  ledger.tieRanges = {{4, 8}, {8, 12}};
  return ledger;
}

constexpr llvm::StringRef JointSetShapeDefect =
    "is not two or more ascending windows including its own";
constexpr llvm::StringRef JointPartnerDefect =
    "which holds no witness recording the same joint window set";
constexpr llvm::StringRef JointTheoremDefect =
    "of one joint repair disagree on its candidate census or key";

} // namespace

TEST(RefoldAlignmentSemanticLedger, WellFormedLedgerHasNoDefect) {
  EXPECT_EQ(WellFormedLedger().audit(), std::nullopt);
}

TEST(RefoldAlignmentSemanticLedger,
     StructureRecoveryDoesNotClaimEquivalentRealization) {
  WellFormedLedger ledger;
  ledger.witnesses[2].anchorEvidence.back().basis =
      AlignmentSemanticAnchorBasis::EquivalentRealizationRepresentative;
  expectDefect(ledger.audit(), BasisDefect);
}

TEST(RefoldAlignmentSemanticLedger, CompleteGroundSetHasNoNecessityBasis) {
  WellFormedLedger ledger;
  ledger.witnesses[0].anchorEvidence.back().basis =
      AlignmentSemanticAnchorBasis::SourcePreservationNecessary;
  expectDefect(ledger.audit(), BasisDefect);
}

TEST(RefoldAlignmentSemanticLedger,
     RequiredAnchorCarrierDoesNotClaimStructureRecovery) {
  WellFormedLedger ledger;
  ledger.witnesses[1].anchorEvidence.back().basis =
      AlignmentSemanticAnchorBasis::
          StructureRespectingTerminalRecoveryRepresentative;
  expectDefect(ledger.audit(), BasisDefect);
}

TEST(RefoldAlignmentSemanticLedger,
     StructureRecoveryDoesNotClaimTheCompleteGroundSet) {
  WellFormedLedger ledger;
  ledger.witnesses[2].candidateDomain =
      AlignmentSemanticCandidateDomain::AllOptimalMaps;
  expectDefect(ledger.audit(), "candidate domain");
}

TEST(RefoldAlignmentSemanticLedger,
     CompleteGroundSetMayBeQuotientedByPlainSubRectangles) {
  WellFormedLedger ledger;
  ledger.witnesses[0].candidateDomain =
      AlignmentSemanticCandidateDomain::PlainQuotientedOptimalMaps;
  EXPECT_FALSE(ledger.audit().has_value());
}

TEST(RefoldAlignmentSemanticLedger,
     OnlyTheCompleteGroundSetMayBeQuotientedByPlainSubRectangles) {
  WellFormedLedger requiredAnchor;
  requiredAnchor.witnesses[1].candidateDomain =
      AlignmentSemanticCandidateDomain::PlainQuotientedOptimalMaps;
  expectDefect(requiredAnchor.audit(), "candidate domain");

  WellFormedLedger structureRecovery;
  structureRecovery.witnesses[2].candidateDomain =
      AlignmentSemanticCandidateDomain::PlainQuotientedOptimalMaps;
  expectDefect(structureRecovery.audit(), "candidate domain");
}

TEST(RefoldAlignmentSemanticLedger, RequiredAnchorCarrierHoldsARealizationKey) {
  WellFormedLedger ledger;
  ledger.witnesses[1].equivalenceKeyKind =
      AlignmentSemanticEquivalenceKeyKind::ConcreteOutput;
  expectDefect(ledger.audit(), "key of the wrong kind");
}

TEST(RefoldAlignmentSemanticLedger, OnlyStructureRecoveryHasARepairScope) {
  WellFormedLedger withScope;
  withScope.witnesses[0].structureRepairScope =
      StructureRepairScope::AllPreferredSubRectangles;
  expectDefect(withScope.audit(), "structure repair scope");

  WellFormedLedger withoutScope;
  withoutScope.witnesses[2].structureRepairScope.reset();
  expectDefect(withoutScope.audit(), "structure repair scope");

  WellFormedLedger withCount;
  withCount.witnesses[0].variedSubRectangleCount = 1;
  expectDefect(withCount.audit(), "varied sub-rectangle count");

  WellFormedLedger withoutCount;
  withoutCount.witnesses[2].variedSubRectangleCount = 0;
  expectDefect(withoutCount.audit(), "varied sub-rectangle count");
}

TEST(RefoldAlignmentSemanticLedger, IncompleteCandidateDomainIsRejected) {
  WellFormedLedger ledger;
  ledger.witnesses[0].candidateDomainCompletelyEnumerated = false;
  expectDefect(ledger.audit(), "incomplete or malformed");
}

TEST(RefoldAlignmentSemanticLedger, WitnessWindowMatchesItsCertification) {
  WellFormedLedger shifted;
  shifted.witnesses[1].aEnd = 9;
  expectDefect(shifted.audit(), "disagrees with certification window 1");

  WellFormedLedger uncertified;
  uncertified.windows[1].status =
      diffutils::LcsWindowCertificationStatus::BudgetExceeded;
  expectDefect(uncertified.audit(), "disagrees with certification window 1");
}

TEST(RefoldAlignmentSemanticLedger, OneWitnessPerWindow) {
  WellFormedLedger ledger;
  ledger.witnesses[1].windowIndex = 0;
  expectDefect(ledger.audit(), "already resolved certification window 0");
}

TEST(RefoldAlignmentSemanticLedger, EvidenceStaysInsideItsWitnessWindow) {
  WellFormedLedger ledger;
  ledger.witnesses[0].anchorEvidence.push_back(
      {5, 5,
       AlignmentSemanticAnchorBasis::EquivalentRealizationRepresentative});
  expectDefect(ledger.audit(), "A-token 5 outside its window");
}

TEST(RefoldAlignmentSemanticLedger,
     StructureRecoveryOwnsATieRangeOverItsWindow) {
  WellFormedLedger ledger;
  ledger.tieRanges = {{0, 4}};
  expectDefect(ledger.audit(), "without a structure-preserving tie range");
}

TEST(RefoldAlignmentSemanticLedger, EveryTieRangeBelongsToARecovery) {
  WellFormedLedger ledger;
  ledger.tieRanges.push_back({0, 4});
  expectDefect(ledger.audit(), "one-to-one");
}

TEST(RefoldAlignmentSemanticLedger, JointRecoveryLedgerHasNoDefect) {
  EXPECT_EQ(jointLedger().audit(), std::nullopt);
}

TEST(RefoldAlignmentSemanticLedger, OnlyAJointRecoveryHasAJointWindowSet) {
  WellFormedLedger withSet;
  withSet.witnesses[2].jointWindowIndices = {1, 2};
  expectDefect(withSet.audit(), "joint window set exactly when");

  WellFormedLedger withoutSet = jointLedger();
  withoutSet.witnesses[1].jointWindowIndices.clear();
  expectDefect(withoutSet.audit(), "joint window set exactly when");
}

TEST(RefoldAlignmentSemanticLedger, JointWindowSetHoldsItsOwnWindowAndAnother) {
  WellFormedLedger alone = jointLedger();
  alone.witnesses[1].jointWindowIndices = {1};
  expectDefect(alone.audit(), JointSetShapeDefect);

  WellFormedLedger descending = jointLedger();
  descending.witnesses[1].jointWindowIndices = {2, 1};
  expectDefect(descending.audit(), JointSetShapeDefect);

  WellFormedLedger repeated = jointLedger();
  repeated.witnesses[1].jointWindowIndices = {1, 1, 2};
  expectDefect(repeated.audit(), JointSetShapeDefect);

  WellFormedLedger withoutOwn = jointLedger();
  withoutOwn.witnesses[1].jointWindowIndices = {0, 2};
  expectDefect(withoutOwn.audit(), JointSetShapeDefect);
}

TEST(RefoldAlignmentSemanticLedger, EveryJointWindowHoldsAWitnessOfTheSet) {
  // Window 1's witness is gone, its anchors left core-forced and its tie range
  // dropped, so every other check passes and window 2's witness is the lone
  // member of its joint theorem.
  WellFormedLedger lone = jointLedger();
  lone.witnesses.erase(lone.witnesses.begin() + 1);
  for (size_t token = 5; token < 8; ++token)
    lone.anchorProofs[token] = diffutils::LcsAnchorProof{
        diffutils::LcsAnchorProofKind::CoreOptimalPathForced, 0};
  lone.tieRanges = {{8, 12}};
  expectDefect(lone.audit(), JointPartnerDefect);

  WellFormedLedger otherSet = jointLedger();
  otherSet.witnesses[2].jointWindowIndices = {0, 2};
  expectDefect(otherSet.audit(), JointPartnerDefect);
}

TEST(RefoldAlignmentSemanticLedger, JointWitnessesShareOneCensusAndKey) {
  WellFormedLedger otherKey = jointLedger();
  otherKey.witnesses[2].equivalenceKey = "other key";
  expectDefect(otherKey.audit(), JointTheoremDefect);

  WellFormedLedger otherCensus = jointLedger();
  otherCensus.witnesses[2].candidateCount = 4;
  otherCensus.witnesses[2].rejectedCandidateCount = 3;
  expectDefect(otherCensus.audit(), JointTheoremDefect);
}

//===-- RefoldLcsBudgetPartitionTests.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the seam schedule of the budget-driven certification
// partitioner.
//
// A rectangle over the byte budget is split at an A row that every optimal
// path crosses at one B frontier. The schedule used to be the arithmetic
// midpoint and, failing that, the producer nomination nearest it. A row beside
// a replaced token is never such a seam: optimal paths order the deletion and
// the insertion both ways, so the row admits two frontiers. When the midpoint
// lands there and is itself the nearest nomination, the schedule held one row,
// and the whole rectangle stayed uncertified with no anchors. On brotli's
// `backward_references.c` that left a 25723-token window as a single hunk.
//
// The midpoint of ten tokens is row 5, and replacing A[5] makes rows 5 and 6
// ambiguous while rows 4 and 7 stay unique. No producer nominations are
// supplied, so the midpoint is the whole original schedule.
//
// The partitioner also confines its row projections to a diagonal band that
// every optimal path is proven to stay in. Ten-token grids are narrower than
// the band's probe, so the band is covered on streams of a few hundred tokens:
// every seam it accepts must be a seam of the whole stream under the unbanded
// projection, and a fully certified partition must force exactly the anchors
// the unpartitioned stream forces.
//
//===----------------------------------------------------------------------===//

#include "source/DiffAlgorithms.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace clang::refold;
using namespace clang::refold::diffutils;

namespace {

/// A token stream and an edited copy, with per-boundary owner-depth costs.
struct EditedStreams {
  std::vector<llvm::StringRef> a;
  std::vector<llvm::StringRef> b;
  std::vector<LcsAGapProvenance> gaps;
};

/// Draw A over a six-symbol alphabet, then B by sparse replacements,
/// deletions, and insertions. A nonzero \p driftBlock also deletes that many A
/// tokens at a quarter of the stream and inserts as many at three quarters, so
/// every optimal path drifts that far off the main diagonal and back.
///
/// Only raw `std::mt19937` output is used, never a standard distribution, so
/// the streams are the same on every platform.
EditedStreams makeEditedStreams(std::mt19937 &random, size_t aTokens,
                                size_t driftBlock) {
  static const llvm::StringRef kept[] = {"k0", "k1", "k2", "k3", "k4", "k5"};
  static const llvm::StringRef fresh[] = {"f0", "f1", "f2", "f3", "f4", "f5"};
  EditedStreams streams;
  for (size_t index = 0; index < aTokens; ++index)
    streams.a.push_back(kept[random() % 6]);

  const size_t driftDelete = aTokens / 4;
  const size_t driftInsert = 3 * aTokens / 4;
  for (size_t index = 0; index < aTokens; ++index) {
    if (index >= driftDelete && index < driftDelete + driftBlock)
      continue;
    if (index == driftInsert) {
      for (size_t inserted = 0; inserted < driftBlock; ++inserted)
        streams.b.push_back(fresh[random() % 6]);
    }
    const unsigned edit = random() % 100;
    if (edit < 3) {
      streams.b.push_back(fresh[random() % 6]);
    } else if (edit < 5) {
      continue;
    } else if (edit < 7) {
      streams.b.push_back(fresh[random() % 6]);
      streams.b.push_back(streams.a[index]);
    } else {
      streams.b.push_back(streams.a[index]);
    }
  }

  streams.gaps.resize(aTokens + 1);
  for (LcsAGapProvenance &gap : streams.gaps)
    gap.ownerDepth = random() % 3;
  return streams;
}

struct BudgetPartitionFixture {
  std::vector<llvm::StringRef> a = {"t0", "t1", "t2", "t3", "t4",
                                    "t5", "t6", "t7", "t8", "t9"};
  std::vector<llvm::StringRef> b = a;
  std::vector<LcsAGapProvenance> gaps = std::vector<LcsAGapProvenance>(11);

  /// Return a budget that refuses the complete ten-by-ten rectangle but
  /// certifies, and may keep an oracle for, any six-by-six window.
  static unsigned long long SixBySixBudget() {
    uint64_t wholeBytes = 0;
    uint64_t childBytes = 0;
    EXPECT_TRUE(getLcsCertificationRequiredBytes(
        10, 10, /*retainCompleteOracle=*/false, wholeBytes));
    EXPECT_TRUE(getLcsCertificationRequiredBytes(
        6, 6, /*retainCompleteOracle=*/true, childBytes));
    EXPECT_LT(childBytes, wholeBytes);
    return childBytes;
  }

  CertifiedLcsResult Certify() const {
    CertifiedLcsResult result;
    EXPECT_TRUE(certifyLcsWindowsWithinBudget(a, b, gaps,
                                              /*candidateABoundaries=*/{},
                                              SixBySixBudget(), result));
    return result;
  }
};

} // namespace

TEST(RefoldLcsBudgetPartition, MidpointBesideReplacementFallsBackToSweptRow) {
  BudgetPartitionFixture fixture;
  fixture.b[5] = "u5";

  LcsBoundaryFrontierProjection midpoint;
  ASSERT_TRUE(projectLcsBoundaryToOptimalBFrontiers(
      fixture.a, 0, 10, fixture.b, 0, 10, /*aBoundary=*/5, fixture.gaps,
      midpoint));
  ASSERT_FALSE(midpoint.ProvesUniqueBoundary());

  const CertifiedLcsResult result = fixture.Certify();
  EXPECT_TRUE(result.allWindowsCertified);
  ASSERT_FALSE(result.certifiedBoundaries.empty());
  EXPECT_EQ(result.certifiedBoundaries.front().aBoundary, 4u);
  EXPECT_EQ(result.certifiedBoundaries.front().bBoundary, 4u);
  EXPECT_EQ(result.certifiedBoundaries.front().proofKind,
            LcsBoundaryProofKind::EveryOptimalPathCrossesState);
  ASSERT_EQ(result.forcedMap.size(), 10u);
  for (int64_t aToken = 0; aToken < 10; ++aToken)
    EXPECT_EQ(result.forcedMap[aToken], aToken == 5 ? -1 : aToken)
        << "aToken=" << aToken;
}

TEST(RefoldLcsBudgetPartition, UniqueMidpointKeepsTheMidpointSeam) {
  BudgetPartitionFixture fixture;
  fixture.b[2] = "u2";

  const CertifiedLcsResult result = fixture.Certify();
  EXPECT_TRUE(result.allWindowsCertified);
  ASSERT_FALSE(result.certifiedBoundaries.empty());
  EXPECT_EQ(result.certifiedBoundaries.front().aBoundary, 5u);
  EXPECT_EQ(result.certifiedBoundaries.front().bBoundary, 5u);
}

TEST(RefoldLcsBudgetPartition, RectangleWithoutSingletonRowStaysUnresolved) {
  BudgetPartitionFixture fixture;
  fixture.b = {"u0", "u1", "u2", "u3", "u4", "u5", "u6", "u7", "u8", "u9"};

  // With no token in common every monotone path is optimal, so every row
  // admits every frontier and no schedule may split the rectangle.
  const CertifiedLcsResult result = fixture.Certify();
  EXPECT_FALSE(result.allWindowsCertified);
  EXPECT_TRUE(result.certifiedBoundaries.empty());
  ASSERT_EQ(result.forcedMap.size(), 10u);
  for (int64_t bToken : result.forcedMap)
    EXPECT_EQ(bToken, -1);
}

TEST(RefoldLcsBudgetPartition, BandedSeamsAreSeamsOfTheWholeStream) {
  std::mt19937 random(20261006u);
  unsigned fullyCertified = 0;
  for (unsigned trial = 0; trial < 12; ++trial) {
    const EditedStreams streams =
        makeEditedStreams(random, 360, trial % 2 == 0 ? 0 : 100);
    const size_t n = streams.a.size();
    const size_t m = streams.b.size();

    CertifiedLcsResult whole;
    ASSERT_TRUE(certifyLcsWindowsWithinBudget(streams.a, streams.b,
                                              streams.gaps, {},
                                              DEFAULT_MAX_BYTES, whole));
    ASSERT_TRUE(whole.allWindowsCertified);

    uint64_t quarterBytes = 0;
    ASSERT_TRUE(getLcsCertificationRequiredBytes(
        n / 4, m / 4, /*retainCompleteOracle=*/true, quarterBytes));
    CertifiedLcsResult partitioned;
    ASSERT_TRUE(certifyLcsWindowsWithinBudget(streams.a, streams.b,
                                              streams.gaps, {}, quarterBytes,
                                              partitioned));
    ASSERT_TRUE(partitioned.globalObjectiveIsExact);
    EXPECT_TRUE(partitioned.globalObjective == whole.globalObjective)
        << "trial=" << trial;
    EXPECT_FALSE(partitioned.certifiedBoundaries.empty()) << "trial=" << trial;

    for (const LcsCertifiedBoundary &seam : partitioned.certifiedBoundaries) {
      LcsBoundaryFrontierProjection projection;
      ASSERT_TRUE(projectLcsBoundaryToOptimalBFrontiers(
          streams.a, 0, n, streams.b, 0, m, seam.aBoundary, streams.gaps,
          projection));
      ASSERT_TRUE(projection.ProvesUniqueBoundary())
          << "trial=" << trial << " aBoundary=" << seam.aBoundary;
      EXPECT_EQ(projection.admissibleBFrontiers.front(), seam.bBoundary)
          << "trial=" << trial << " aBoundary=" << seam.aBoundary;
    }

    if (!partitioned.allWindowsCertified)
      continue;
    ++fullyCertified;
    EXPECT_EQ(partitioned.forcedMap, whole.forcedMap) << "trial=" << trial;
  }
  EXPECT_GE(fullyCertified, 6u);
}

TEST(RefoldLcsBudgetPartition, BandHoldsAnOptimalPathAlongItsEdge) {
  // B deletes A[90, 140) and appends 50 fresh tokens, so every deletion comes
  // before every insertion and the only optimal path runs along diagonal
  // `L* - n = -50` from A boundary 140 onward. The drift is within the probe's
  // margin, so the probe finds `L*` itself and that diagonal is the band's
  // lower edge: a band one diagonal narrower loses the path.
  std::vector<std::string> spellings;
  for (unsigned token = 0; token < 410; ++token)
    spellings.push_back("t" + std::to_string(token));
  std::vector<llvm::StringRef> a(spellings.begin(), spellings.begin() + 360);
  std::vector<llvm::StringRef> b(spellings.begin(), spellings.begin() + 90);
  b.insert(b.end(), spellings.begin() + 140, spellings.end());
  const std::vector<LcsAGapProvenance> gaps(a.size() + 1);

  CertifiedLcsResult whole;
  ASSERT_TRUE(certifyLcsWindowsWithinBudget(a, b, gaps, {}, DEFAULT_MAX_BYTES,
                                            whole));
  ASSERT_TRUE(whole.allWindowsCertified);

  uint64_t quarterBytes = 0;
  ASSERT_TRUE(getLcsCertificationRequiredBytes(
      a.size() / 4, b.size() / 4, /*retainCompleteOracle=*/true, quarterBytes));
  CertifiedLcsResult partitioned;
  ASSERT_TRUE(certifyLcsWindowsWithinBudget(a, b, gaps, {}, quarterBytes,
                                            partitioned));
  EXPECT_TRUE(partitioned.allWindowsCertified);
  EXPECT_TRUE(partitioned.globalObjective == whole.globalObjective);
  EXPECT_EQ(partitioned.forcedMap, whole.forcedMap);
  ASSERT_FALSE(partitioned.certifiedBoundaries.empty());
  for (const LcsCertifiedBoundary &seam : partitioned.certifiedBoundaries) {
    LcsBoundaryFrontierProjection projection;
    ASSERT_TRUE(projectLcsBoundaryToOptimalBFrontiers(
        a, 0, a.size(), b, 0, b.size(), seam.aBoundary, gaps, projection));
    ASSERT_TRUE(projection.ProvesUniqueBoundary())
        << "aBoundary=" << seam.aBoundary;
    EXPECT_EQ(projection.admissibleBFrontiers.front(), seam.bBoundary)
        << "aBoundary=" << seam.aBoundary;
  }
}

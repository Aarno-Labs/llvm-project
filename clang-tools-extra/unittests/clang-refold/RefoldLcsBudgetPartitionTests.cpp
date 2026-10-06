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
//===----------------------------------------------------------------------===//

#include "source/DiffAlgorithms.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <vector>

using namespace clang::refold;
using namespace clang::refold::diffutils;

namespace {

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

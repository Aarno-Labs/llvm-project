//===-- RefoldLcsOracleObjectiveTests.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the contract of
// `OptimalTokenAlignmentOracle::ObjectiveForWindow` when the byte budget
// declines a window query.
//
// The legacy boundary proposal decides joint core optimality by summing window
// objectives between its anchors and comparing the sum with the global
// objective.  The query used to return a default `{0, 0}` objective when the
// window tables did not fit beside the retained oracle, and that placeholder
// was summed as if it were exact.  With A = `x`, B = `x x`, and an owner-depth
// gap cost of 5 at A's right boundary, the map `x -> B[0]` costs 5 while the
// optimum costs 0; dropping the refused suffix window's cost made the map look
// jointly optimal.
//
// The regression is only reachable in a narrow byte window -- the global oracle
// must be retained while the suffix window is refused -- so it is pinned here
// against the oracle directly instead of through a lit test.
//
//===----------------------------------------------------------------------===//

#include "source/DiffAlgorithms.h"

#include "gtest/gtest.h"

#include <optional>
#include <vector>

using namespace clang::refold;

namespace {

struct RepeatedTokenGapCostFixture {
  std::vector<llvm::StringRef> a = {"x"};
  std::vector<llvm::StringRef> b = {"x", "x"};
  std::vector<diffutils::LcsAGapProvenance> gaps =
      std::vector<diffutils::LcsAGapProvenance>(2);

  RepeatedTokenGapCostFixture() {
    gaps[0].ownerDepth = 0;
    gaps[1].ownerDepth = 5;
  }

  bool Certify(unsigned long long maxBytes,
               diffutils::CertifiedLcsResult &result) const {
    diffutils::certifyLcsWindowsWithinBudget(a, b, gaps,
                                             /*candidateABoundaries=*/{},
                                             maxBytes, result);
    return result.HasCompleteSemanticOracleForWindow(/*windowIndex=*/0);
  }
};

} // namespace

TEST(RefoldLcsOracleObjective, AmpleBudgetReportsExactWindowCost) {
  RepeatedTokenGapCostFixture fixture;
  diffutils::CertifiedLcsResult result;
  ASSERT_TRUE(fixture.Certify(diffutils::DEFAULT_MAX_BYTES, result));

  std::optional<diffutils::LcsObjective> suffix =
      result.oracle.ObjectiveForWindow(1, 1, 1, 2);
  ASSERT_TRUE(suffix.has_value());
  EXPECT_EQ(suffix->matchedTokenCount, 0u);
  EXPECT_EQ(suffix->ownerDepthCost, 5u);
}

TEST(RefoldLcsOracleObjective, RefusedWindowIsNotAZeroObjective) {
  RepeatedTokenGapCostFixture fixture;

  // Find a budget that retains the complete global oracle but declines the
  // one-token suffix window.  Every such budget must report the refusal rather
  // than an objective, so the caller cannot sum it.
  bool sawRetainedOracleWithRefusedWindow = false;
  for (unsigned long long maxBytes = 1; maxBytes <= 4096; ++maxBytes) {
    diffutils::CertifiedLcsResult result;
    if (!fixture.Certify(maxBytes, result))
      continue;

    std::optional<diffutils::LcsObjective> suffix =
        result.oracle.ObjectiveForWindow(1, 1, 1, 2);
    if (suffix) {
      // Once the window fits, the answer must be the exact cost, never a
      // placeholder.
      EXPECT_EQ(suffix->ownerDepthCost, 5u) << "maxBytes=" << maxBytes;
      continue;
    }
    sawRetainedOracleWithRefusedWindow = true;
  }
  EXPECT_TRUE(sawRetainedOracleWithRefusedWindow);
}

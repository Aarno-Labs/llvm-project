//===-- RefoldHirschbergProgressTests.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for termination of the linear-space LCS recursion.
//
// Hirschberg splits A in half and recurses on the two induced subproblems.
// With one A token `mid` is `n / 2 == 0`, so the left subproblem is empty and
// the right one is handed back the identical box.  The exact-DP base case
// normally absorbs that, but it is guarded by a fixed cell count: at one A
// token the box holds `2 * (|B| + 1)` cells, which exceeds the guard once B
// reaches 524288 tokens.  Past that point the recursion made no progress and
// overflowed the stack.
//
// The shape below is minimal: two A tokens that appear nowhere in B, so every
// split is equally good, the smallest is kept for determinism, and the whole
// of B descends into a single-A-token subproblem.  Both the weighted and the
// plain recursion are covered because they carry the same split.
//
// A byte budget far below the quadratic table forces the linear-space path;
// the inner guard is a fixed cell count, so B must still be large in absolute
// terms for the recursion to reach the non-progressing state.
//
//===----------------------------------------------------------------------===//

#include "source/DiffAlgorithms.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <vector>

using namespace clang::refold;
using namespace clang::refold::diffutils;

namespace {

/// One A token past this many B tokens skips the exact-DP base case, because
/// `2 * (kNonProgressingBWidth + 1)` exceeds the recursion's cell guard.
constexpr size_t kNonProgressingBWidth = 524288;

/// A byte budget small enough that the quadratic table is always refused.
constexpr unsigned long long kForceLinearSpaceBytes = 1024;

struct SingleATokenOverLargeBFixture {
  std::vector<llvm::StringRef> a = {"unmatched_first", "unmatched_second"};
  std::vector<llvm::StringRef> b;
  std::vector<uint32_t> ownerDepthGap = {0, 0, 0};

  SingleATokenOverLargeBFixture() {
    // No A token occurs in B, so no split is better than another. Ties keep
    // the smallest split index, which leaves all of B under one A token.
    b.assign(kNonProgressingBWidth, llvm::StringRef("b_filler"));
  }
};

TEST(RefoldHirschbergProgress, WeightedRecursionTerminatesOnSingleAToken) {
  SingleATokenOverLargeBFixture fixture;

  const std::vector<int64_t> map =
      lcsMapAB(fixture.a, fixture.b, fixture.ownerDepthGap,
               kForceLinearSpaceBytes);

  ASSERT_EQ(map.size(), fixture.a.size());
  for (const int64_t mapped : map)
    EXPECT_EQ(mapped, -1);
}

TEST(RefoldHirschbergProgress, PlainRecursionTerminatesOnSingleAToken) {
  SingleATokenOverLargeBFixture fixture;

  const std::vector<int64_t> map =
      lcsMapAB(fixture.a, fixture.b, kForceLinearSpaceBytes);

  ASSERT_EQ(map.size(), fixture.a.size());
  for (const int64_t mapped : map)
    EXPECT_EQ(mapped, -1);
}

} // namespace

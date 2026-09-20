//===-- RefoldLineControlLookbackTests.cpp ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the search boundary of
// `findLastLineControlDirectiveRangeBefore`.
//
// The search used to stop a fixed 16 KiB before the insertion point.  Its only
// consumer asks whether the previous line-control directive lies inside the
// conditional group being rejoined, and answers "no" when the search finds
// nothing -- which suppresses the post-join `#line`.  For a group whose emitted
// body is larger than that window the suppression is wrong: the directive is
// inside the group, the window just never reached it.  Apparent file/line
// equality does not establish the state at a join point when the prior state
// was set inside an arm that does not dominate it.
//
// The fixed window also truncated the line it stopped inside, handing a
// fragment to the directive predicate.  That both hides a real directive whose
// line straddles the boundary and lets the tail of an ordinary line be read as
// one.
//
// The bound is now supplied by the caller as a proof boundary, so these are
// pinned against the helper directly: no lit shape emits enough output for a
// fixed window to bind.
//
//===----------------------------------------------------------------------===//

#include "line-control/LineControlEditHelpers.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

using namespace clang::refold;

namespace {

/// The fixed window the search used to stop at.
constexpr size_t kOldFixedWindowBytes = 16 * 1024;

/// Wider than the window the search used to stop at.
constexpr size_t kPaddingWiderThanOldWindow = 20 * 1024;

/// Build `<directive>\n` followed by enough ordinary lines to push the
/// directive further back than the old fixed window.
std::string buildPaddedStreamAfterDirective(llvm::StringRef directive,
                                            size_t paddingBytes) {
  std::string out;
  out.append(directive.str());
  out.push_back('\n');
  while (out.size() < paddingBytes) {
    out.append("int filler_line = 0;");
    out.push_back('\n');
  }
  return out;
}

TEST(RefoldLineControlLookback, FindsDirectiveFurtherBackThanOldFixedWindow) {
  const std::string out =
      buildPaddedStreamAfterDirective("# 42 \"header.h\"",
                                      kPaddingWiderThanOldWindow);
  ASSERT_GT(out.size(), kPaddingWiderThanOldWindow);

  const std::optional<std::pair<uint64_t, uint64_t>> found =
      findLastLineControlDirectiveRangeBefore(out, out.size(),
                                              /*scanLowerBound=*/0);

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->first, 0u);
  EXPECT_EQ(llvm::StringRef(out).slice(found->first, found->second).rtrim(),
            "# 42 \"header.h\"");
}

TEST(RefoldLineControlLookback, IgnoresDirectiveBeforeTheProofBoundary) {
  const std::string out =
      buildPaddedStreamAfterDirective("# 42 \"header.h\"", 512);

  // The directive occupies the first line, so a bound past it means no
  // directive lies wholly inside the scanned region.
  const std::optional<std::pair<uint64_t, uint64_t>> found =
      findLastLineControlDirectiveRangeBefore(out, out.size(),
                                              /*scanLowerBound=*/64);

  EXPECT_FALSE(found.has_value());
}

TEST(RefoldLineControlLookback, DoesNotReadALineStraddlingTheBoundAsDirective) {
  // An ordinary line whose tail alone would satisfy the directive predicate.
  // The bound is placed exactly on that tail and the stream padded so the tail
  // also sits exactly one old-window width before the end -- the position at
  // which the fixed window used to cut the line and expose the tail as if it
  // began one.
  const std::string straddlingLine = "int a = 0; # 9 \"spoofed.h\"";
  const size_t boundInsideLine = straddlingLine.find('#');
  ASSERT_NE(boundInsideLine, std::string::npos);

  std::string out = straddlingLine;
  out.push_back('\n');
  while (out.size() < boundInsideLine + kOldFixedWindowBytes)
    out.append("int filler_line = 0;\n");
  out.resize(boundInsideLine + kOldFixedWindowBytes);

  const std::optional<std::pair<uint64_t, uint64_t>> found =
      findLastLineControlDirectiveRangeBefore(
          out, out.size(), /*scanLowerBound=*/boundInsideLine);

  EXPECT_FALSE(found.has_value());
}

} // namespace

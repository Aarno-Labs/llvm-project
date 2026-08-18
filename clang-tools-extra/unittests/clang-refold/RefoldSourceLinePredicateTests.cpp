//===-- RefoldSourceLinePredicateTests.cpp ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `intervalIsAloneOnItsLine` decides whether an interval may be rewritten into
// a preprocessing directive, because a directive is one only when it begins a
// line.  The pragma-once guard rewrite consults it before replacing a
// `_Pragma("once")` with a guard `#define`.
//
// It is pinned here rather than through lit because the interesting half is the
// refusal, and a refused guard falls through to the terminal carrier: a lit
// test for it would have to pin the raw edited preprocessed stream as its
// expected output, asserting that surrendering the translation unit is the
// desired result.  The predicate is a pure function over bytes, so its whole
// contract is testable directly.
//
//===----------------------------------------------------------------------===//

#include "util/StringUtils.h"

#include "gtest/gtest.h"

using namespace clang;
using namespace clang::refold;

namespace {

/// Return the predicate applied to the first occurrence of \p needle.
bool aloneAt(llvm::StringRef text, llvm::StringRef needle) {
  const size_t begin = text.find(needle);
  EXPECT_NE(begin, llvm::StringRef::npos) << "needle not present in fixture";
  if (begin == llvm::StringRef::npos)
    return false;
  return stringutils::intervalIsAloneOnItsLine(text, begin,
                                               begin + needle.size());
}

TEST(RefoldSourceLinePredicate, AloneOnItsOwnLine) {
  EXPECT_TRUE(aloneAt("int a = 1;\n_Pragma(\"once\")\nint b = 2;\n",
                      "_Pragma(\"once\")"));
}

TEST(RefoldSourceLinePredicate, AloneWhenOnlySurroundedByHorizontalSpace) {
  EXPECT_TRUE(aloneAt("int a = 1;\n  \t_Pragma(\"once\")\t  \nint b = 2;\n",
                      "_Pragma(\"once\")"));
}

TEST(RefoldSourceLinePredicate, AloneAtStartOfBuffer) {
  EXPECT_TRUE(aloneAt("_Pragma(\"once\")\nint a = 1;\n", "_Pragma(\"once\")"));
}

TEST(RefoldSourceLinePredicate, AloneOnAnUnterminatedFinalLine) {
  EXPECT_TRUE(aloneAt("int a = 1;\n_Pragma(\"once\")", "_Pragma(\"once\")"));
}

TEST(RefoldSourceLinePredicate, AloneUnderCRLF) {
  EXPECT_TRUE(aloneAt("int a = 1;\r\n_Pragma(\"once\")\r\nint b = 2;\r\n",
                      "_Pragma(\"once\")"));
}

TEST(RefoldSourceLinePredicate, NotAloneWithSourceBefore) {
  EXPECT_FALSE(aloneAt("int lead = 1; _Pragma(\"once\")\nint b = 2;\n",
                       "_Pragma(\"once\")"));
}

TEST(RefoldSourceLinePredicate, NotAloneWithSourceAfter) {
  EXPECT_FALSE(aloneAt("_Pragma(\"once\") int trail = 1;\nint b = 2;\n",
                       "_Pragma(\"once\")"));
}

TEST(RefoldSourceLinePredicate, NotAloneWithSourceOnBothSides) {
  EXPECT_FALSE(aloneAt("int a = 1; _Pragma(\"once\") int b = 2;\n",
                       "_Pragma(\"once\")"));
}

// A comment is not whitespace to this predicate.  Preprocessing would replace
// it with a space, but the rewrite copies bytes, so a `#define` emitted here
// would land after the comment's own bytes rather than at a line start.
TEST(RefoldSourceLinePredicate, NotAloneWhenACommentPrecedesOnTheSameLine) {
  EXPECT_FALSE(aloneAt("/* c */ _Pragma(\"once\")\nint b = 2;\n",
                       "_Pragma(\"once\")"));
}

TEST(RefoldSourceLinePredicate, EmptyRangeIsNotAlone) {
  EXPECT_FALSE(stringutils::intervalIsAloneOnItsLine("\nabc\n", 1, 1));
}

TEST(RefoldSourceLinePredicate, InvertedRangeIsNotAlone) {
  EXPECT_FALSE(stringutils::intervalIsAloneOnItsLine("\nabc\n", 3, 1));
}

TEST(RefoldSourceLinePredicate, RangePastTheEndIsNotAlone) {
  EXPECT_FALSE(stringutils::intervalIsAloneOnItsLine("\nabc\n", 1, 99));
}

} // namespace

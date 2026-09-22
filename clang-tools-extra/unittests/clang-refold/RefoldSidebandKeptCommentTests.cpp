//===-- RefoldSidebandKeptCommentTests.cpp ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// When B deletes a sideband pragma, `keptCommentsOfDeletedSidebandSite` gives
// the text that replaces the pragma's source site, so the comments on that
// site survive.  Lit pins the common shape, a block comment trailing a
// `#pragma` line.  The refusal -- a site that ends in a line comment but not
// in a newline, which only a mid-line `_Pragma` operator produces -- is pinned
// here, together with the lexical cases a byte search would get wrong.
//
//===----------------------------------------------------------------------===//

#include "sideband/RefoldSidebandPragmaEdits.h"

#include "clang/Basic/LangOptions.h"

#include "gtest/gtest.h"

using namespace clang;
using namespace clang::refold;

namespace {

/// Return the kept text for \p site under C11 lexing.
std::string keptFor(llvm::StringRef site) {
  LangOptions lang;
  lang.C11 = true;
  lang.LineComment = true;
  return keptCommentsOfDeletedSidebandSite(site, lang);
}

TEST(RefoldSidebandKeptComment, TrailingBlockCommentKeepsItsLine) {
  EXPECT_EQ(keptFor("#pragma vendor note /* why */\n"), "/* why */\n");
}

TEST(RefoldSidebandKeptComment, TrailingLineCommentKeepsItsLine) {
  EXPECT_EQ(keptFor("#pragma vendor note // why\n"), "// why\n");
}

TEST(RefoldSidebandKeptComment, EveryCommentIsKeptInOrder) {
  EXPECT_EQ(keptFor("/* a */ #pragma vendor /* b */ note // c\n"),
            "/* a */ /* b */ // c\n");
}

TEST(RefoldSidebandKeptComment, SiteWithoutCommentKeepsNothing) {
  EXPECT_EQ(keptFor("#pragma vendor note\n"), "");
}

TEST(RefoldSidebandKeptComment, CommentSpellingInsideAStringIsNotAComment) {
  EXPECT_EQ(keptFor("#pragma message(\"/* not a comment */\")\n"), "");
}

TEST(RefoldSidebandKeptComment, MidLineOperatorKeepsBlockCommentInline) {
  EXPECT_EQ(keptFor("_Pragma(/* why */ \"vendor note\")"), "/* why */");
}

TEST(RefoldSidebandKeptComment, LineCommentWithoutNewlineIsRefused) {
  // Keeping `// why` without its newline would comment out the rest of the
  // line the operator sat on.
  EXPECT_EQ(keptFor("_Pragma( // why\n\"vendor note\")"), "");
}

} // namespace

//===-- RefoldRepairIdentityTests.cpp --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the identity that decides "one source repair, or two?".
//
// The witness resolver treats several complete proof certificates for one
// concrete repair as proof-certificate ambiguity and proceeds; certificates
// describing *different* repairs fail closed.  Everything therefore rests on
// telling those apart, and the identity used for it was once
// `payloadPreview` -- a rendering clipped to 120 bytes with its length
// appended.  Two replacements agreeing on their first 120 bytes and sharing a
// length render identically, so the resolver read them as one repair and one
// was silently accepted in place of the other.
//
// No lit test reaches this.  Across the suite, two complete certificates
// describing different repairs occurs zero times; the conflation needs that
// case *and* a 120-byte prefix collision on top of it.  So the property is
// pinned here: the rendering really does conflate, and the identity really
// does not.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldProofVocabulary.h"
#include "util/StringUtils.h"

#include "gtest/gtest.h"

#include <string>

using namespace clang::refold;

namespace {

/// Two replacements that a 120-byte clip cannot tell apart: same first 120
/// bytes, same total length, differing well past the clip point.
std::string longRepairText(char tail) {
  std::string text(120, 'x');
  text.append(40, tail);
  return text;
}

TEST(RefoldRepairIdentity, ClippedRenderingConflatesDistinctRepairs) {
  // The defect, stated directly.  This is not a claim about how the preview
  // *should* behave -- clipping is right for a diagnostic -- but about why it
  // cannot be the thing an admissibility decision compares.
  const std::string first = longRepairText('a');
  const std::string second = longRepairText('b');
  ASSERT_NE(first, second);
  ASSERT_EQ(first.size(), second.size());

  EXPECT_EQ(stringutils::showWsWithClip(first, 120),
            stringutils::showWsWithClip(second, 120));
}

TEST(RefoldRepairIdentity, IdentityDistinguishesWhatTheRenderingConflates) {
  // The fix.  Same two replacements, same owner, same source range: the only
  // difference is bytes the clip discarded, and the identity keeps it.
  const EmittedRepairIdentity first{10, 20, longRepairText('a')};
  const EmittedRepairIdentity second{10, 20, longRepairText('b')};

  EXPECT_NE(first, second);
  EXPECT_FALSE(first == second);
}

TEST(RefoldRepairIdentity, IdentityAgreesOnOneRepair) {
  // The case the resolver must keep proceeding on: several certificates for
  // one concrete repair.  Tightening the identity must not turn these into
  // fail-closed refusals, which is what the 23 suite tests that reach
  // `single-source-repair-multiple-proof-classes` depend on.
  const EmittedRepairIdentity first{10, 20, longRepairText('a')};
  const EmittedRepairIdentity second{10, 20, longRepairText('a')};

  EXPECT_EQ(first, second);
}

TEST(RefoldRepairIdentity, RangeParticipatesInIdentity) {
  // Identical bytes written over a different span are a different repair.
  const std::string text = "int x;";
  EXPECT_NE((EmittedRepairIdentity{10, 20, text}),
            (EmittedRepairIdentity{10, 21, text}));
  EXPECT_NE((EmittedRepairIdentity{10, 20, text}),
            (EmittedRepairIdentity{11, 20, text}));
}

TEST(RefoldRepairIdentity, EmptyRepairIsStillARepair) {
  // A pure deletion emits no bytes, which is a repair like any other and must
  // not be confused with "no identity".  Absence is carried by the optional
  // around this type, never by an empty text.
  EXPECT_EQ((EmittedRepairIdentity{10, 20, std::string()}),
            (EmittedRepairIdentity{10, 20, std::string()}));
  EXPECT_NE((EmittedRepairIdentity{10, 20, std::string()}),
            (EmittedRepairIdentity{10, 21, std::string()}));
}

} // namespace

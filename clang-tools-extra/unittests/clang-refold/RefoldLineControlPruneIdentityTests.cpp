//===-- RefoldLineControlPruneIdentityTests.cpp ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the identity guard in `PruneFinalLineControlDirectives`.
//
// A pruning candidate is minted around one synthetic directive and its offsets
// are then shifted through every splice of its buffer into a parent.  A missed
// shift leaves the range over the wrong bytes.  The pruner used to check only
// that the range held *some* complete line-control directive, so a range that
// drifted exactly onto a different `#line` passed and that directive was
// deleted.  The candidate now records the exact spelling it was minted around
// and the range must still hold it.
//
// The displacement cannot be produced from a lit shape without reintroducing
// the bookkeeping bug it guards against, so it is injected here.  Every
// validation callback accepts unconditionally: the guard must refuse on its
// own, before the executable oracle is consulted.
//
//===----------------------------------------------------------------------===//

#include "line-control/FinalLineControlModel.h"
#include "line-control/LineControlEditHelpers.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace clang::refold;

namespace {

/// Two distinct synthetic directives of equal length, so a range displaced
/// by the gap between them lands exactly on the other complete directive.
constexpr llvm::StringLiteral kFirstDirective = "#line 10 \"a.c\"\n";
constexpr llvm::StringLiteral kSecondDirective = "#line 20 \"b.c\"\n";
constexpr llvm::StringLiteral kBetween = "int x;\n";
constexpr llvm::StringLiteral kAfter = "int y;\n";

std::string buildStream() {
  return (kFirstDirective + kBetween + kSecondDirective + kAfter).str();
}

uint64_t secondDirectiveBegin() {
  return kFirstDirective.size() + kBetween.size();
}

/// A removable candidate whose required-state rules cannot fire, so only the
/// range guard decides whether the pruner attempts the deletion.
FinalLineControlPruneCandidate makeCandidate(uint64_t begin,
                                             std::string spelling) {
  const uint64_t end = begin + kFirstDirective.size();
  return makeSyntheticLineControlPruneCandidate(
      begin, end, std::move(spelling),
      FinalLineDirective::Origin::SyntheticNewlineResync,
      FinalLineControlOwnerKey("a.c", std::nullopt),
      FinalLineControlObligation::CosmeticSyntheticResync);
}

bool acceptEveryDeletion(llvm::StringRef, llvm::StringRef, std::string &) {
  return true;
}

TEST(RefoldLineControlPruneIdentityTest, InPlaceCandidateIsDeleted) {
  // Control for the refusals below: a candidate that still addresses its own
  // directive is deleted, so a refusal is the guard's doing.
  const std::string stream = buildStream();
  FinalLineControlPruneResult result = PruneFinalLineControlDirectives(
      stream, {makeCandidate(0, kFirstDirective.str())}, acceptEveryDeletion);

  EXPECT_TRUE(result.changed);
  EXPECT_EQ(result.output, (kBetween + kSecondDirective + kAfter).str());
}

TEST(RefoldLineControlPruneIdentityTest,
     CandidateDisplacedOntoDifferentDirectiveIsRefused) {
  // The range holds one complete, well-formed `#line` -- just not the one the
  // candidate was minted around.  A shape check alone deletes it.
  const std::string stream = buildStream();
  FinalLineControlPruneResult result = PruneFinalLineControlDirectives(
      stream, {makeCandidate(secondDirectiveBegin(), kFirstDirective.str())},
      acceptEveryDeletion);

  EXPECT_FALSE(result.changed);
  EXPECT_TRUE(result.removedRanges.empty());
  EXPECT_EQ(result.output, stream);
}

TEST(RefoldLineControlPruneIdentityTest, CandidateWithoutSpellingIsRefused) {
  // No recorded spelling means no identity.  The range is correct, but the
  // pruner must not fall back to accepting any directive-shaped bytes.
  const std::string stream = buildStream();
  FinalLineControlPruneResult result = PruneFinalLineControlDirectives(
      stream, {makeCandidate(0, std::string())}, acceptEveryDeletion);

  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.output, stream);
}

TEST(RefoldLineControlPruneIdentityTest,
     CandidatesDisagreeingOnSpellingAtOneRangeAreRefused) {
  // Two mints claim one range for different directives.  At most one can be
  // right and nothing records which, so the merged candidate is refused
  // whichever order canonicalization visits them in.
  const std::string stream = buildStream();
  const uint64_t begin = secondDirectiveBegin();
  FinalLineControlPruneResult result = PruneFinalLineControlDirectives(
      stream,
      {makeCandidate(begin, kSecondDirective.str()),
       makeCandidate(begin, kFirstDirective.str())},
      acceptEveryDeletion);

  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.output, stream);
}

TEST(RefoldLineControlPruneIdentityTest, DeduplicationKeepsOnlyAgreedSpelling) {
  std::vector<FinalLineControlPruneCandidate> agreeing = {
      makeCandidate(0, kFirstDirective.str()),
      makeCandidate(0, kFirstDirective.str())};
  deduplicateLineControlPruneCandidates(agreeing);
  ASSERT_EQ(agreeing.size(), 1u);
  EXPECT_EQ(agreeing.front().directiveSpelling, kFirstDirective);

  std::vector<FinalLineControlPruneCandidate> conflicting = {
      makeCandidate(0, kFirstDirective.str()),
      makeCandidate(0, kSecondDirective.str())};
  deduplicateLineControlPruneCandidates(conflicting);
  ASSERT_EQ(conflicting.size(), 1u);
  EXPECT_TRUE(conflicting.front().directiveSpelling.empty());
}

} // namespace

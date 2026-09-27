//===-- RefoldAlignmentSemanticResolverTests.cpp ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the theorem boundary of `RefoldAlignmentSemanticResolver`'s
// structure-respecting terminal recovery, and for the window-local metadata
// each committed window records.
//
// Most of these shapes are hard to reach from C source: a decline leaves the
// window at its core-forced anchors, which in lit is only visible as a refused
// refold.  They are driven here against the real certifier over small lexeme
// streams, with the one expensive dependency -- a complete refold per
// candidate map -- replaced by a scripted simulation.  Each test asserts the
// specific outcome, so removing the rule it pins fails that test.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldAlignmentSemanticResolver.h"

#include "llvm/ADT/StringExtras.h"

#include "gtest/gtest.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace clang::refold;

namespace {

using Resolution = RefoldAlignmentSemanticResolver::ResolutionResult;
using TokenRange = std::pair<uint64_t, uint64_t>;
using Simulation = AlignmentSemanticSimulationResult;

/// Return an accepted simulation whose output is \p key.
Simulation accepted(std::string key) {
  Simulation result;
  result.accepted = true;
  result.disposition = AlignmentSemanticSimulationDisposition::Accepted;
  result.realizationEquivalenceKey = key;
  result.concreteOutputEquivalenceKey = std::move(key);
  return result;
}

/// Return a simulation that requested terminal fallback, naming \p ranges.
Simulation terminal(std::vector<std::optional<TokenRange>> ranges = {
                        std::nullopt}) {
  Simulation result;
  result.disposition = AlignmentSemanticSimulationDisposition::TerminalFallback;
  result.terminalRequestATokenRanges = std::move(ranges);
  return result;
}

Simulation proofIncomplete() {
  Simulation result;
  result.disposition = AlignmentSemanticSimulationDisposition::ProofIncomplete;
  return result;
}

/// Return the A tokens in `[begin, end)` that \p selection leaves unmatched,
/// comma-separated: the fake simulations below identify a map by them.
std::string unmatchedIn(const AlignmentSelectionOverride &selection,
                        uint64_t begin, uint64_t end) {
  std::vector<std::string> tokens;
  for (uint64_t aToken = begin; aToken < end; ++aToken)
    if (selection.selectedMap[aToken] < 0)
      tokens.push_back(std::to_string(aToken));
  return llvm::join(tokens, ",");
}

/// Return \p selection's structure-preserving tie ranges, spelled for a key.
std::string tieRangesOf(const AlignmentSelectionOverride &selection) {
  std::vector<std::string> ranges;
  for (const TokenRange &range : selection.structurePreservingTieRanges)
    ranges.push_back("[" + std::to_string(range.first) + "," +
                     std::to_string(range.second) + ")");
  return llvm::join(ranges, "");
}

/// A resolver over the real certification of two lexeme streams, with every
/// candidate realization answered by `simulate`.
///
/// No B gap provenance is supplied, so the legacy boundary proposal is
/// unreachable and a window every complete-ground-set rule declines reaches
/// the structure-respecting repair directly.
class ResolverHarness {
public:
  ResolverHarness(std::vector<llvm::StringRef> a,
                  std::vector<llvm::StringRef> b,
                  std::vector<uint64_t> candidateABoundaries = {})
      : a_(std::move(a)), b_(std::move(b)), aGaps_(a_.size() + 1) {
    diffutils::certifyLcsWindowsIndependently(
        a_, b_, aGaps_, candidateABoundaries, diffutils::DEFAULT_MAX_BYTES,
        core_);
  }

  /// Protected A boundaries the repair's first key reads.
  std::vector<uint64_t> protectedBoundaries;
  /// A tokens that begin, and that end, their source line.
  std::set<uint64_t> beginsLine;
  std::set<uint64_t> endsLine;
  /// The scripted realization of one candidate selection.
  std::function<Simulation(const AlignmentSelectionOverride &)> simulate;
  /// Every selection realized, in order.
  std::vector<AlignmentSelectionOverride> realized;

  const diffutils::CertifiedLcsResult &Core() const { return core_; }

  /// Return the A range of certification window \p windowIndex.
  TokenRange WindowARange(size_t windowIndex) const {
    const diffutils::LcsCertificationWindow &window =
        core_.certificationWindows[windowIndex];
    return {window.aBegin, window.aEnd};
  }

  Resolution Resolve() {
    RefoldAlignmentSemanticResolver resolver(
        RefoldAlignmentSemanticResolver::Dependencies{
            a_, b_, core_, aGaps_, /*bGapProvenance=*/{},
            [this](const AlignmentSelectionOverride &selection) {
              realized.push_back(selection);
              return simulate(selection);
            },
            [this](size_t windowIndex) {
              return diffutils::retainCertifiedWindowOracle(
                  a_, b_, aGaps_, diffutils::DEFAULT_MAX_BYTES, windowIndex,
                  core_);
            },
            /*releaseWindowOracle=*/{},
            [this]() { return protectedBoundaries; },
            AlignmentSourceLayoutQueries{
                [this](uint64_t aToken) {
                  return beginsLine.count(aToken) != 0;
                },
                [this](uint64_t aToken) { return endsLine.count(aToken) != 0; },
                [](uint64_t, uint64_t) { return std::vector<uint64_t>(); }}});
    return resolver.Resolve();
  }

private:
  std::vector<llvm::StringRef> a_;
  std::vector<llvm::StringRef> b_;
  std::vector<diffutils::LcsAGapProvenance> aGaps_;
  diffutils::CertifiedLcsResult core_;
};

/// `int y; #define A static int x; #define B static int z;` with the
/// declaration of `x` deleted, as lexemes starting at \p offset.
///
/// The repeated `;` and `static int` give four optimal maps, the deletion slid
/// by one token: runs [2,6), [3,7), [4,8) and [5,9).  The core-forced map
/// leaves [2,9) unmatched.  The `#define`s sit at A gaps 3 and 7.
const std::vector<llvm::StringRef> StructureShapeA = {
    "int", "y", ";", "static", "int", "x", ";", "static", "int", "z", ";"};
const std::vector<llvm::StringRef> StructureShapeB = {"int", "y", ";", "static",
                                                      "int", "z", ";"};

/// The structure shape on its own, with both `#define`s protected: only the
/// run [3,7) straddles neither, so the repair keeps that one map.
ResolverHarness structureShape() {
  ResolverHarness harness(StructureShapeA, StructureShapeB);
  harness.protectedBoundaries = {3, 7};
  return harness;
}

/// Answer the structure shape's decline configuration with \p decline and
/// every other map with an output unique to its unmatched run, so that both
/// complete-ground-set rules decline.
std::function<Simulation(const AlignmentSelectionOverride &)>
distinctOutputsUnless(Simulation decline) {
  return [decline](const AlignmentSelectionOverride &selection) {
    const std::string run = unmatchedIn(selection, 0, 11);
    if (run == "2,3,4,5,6,7,8")
      return decline;
    return accepted("output of " + run);
  };
}

TEST(RefoldAlignmentSemanticResolver, StructureRecoveryRecordsItsOwnTheorem) {
  ResolverHarness harness = structureShape();
  harness.simulate = distinctOutputsUnless(terminal());

  const Resolution resolution = harness.Resolve();
  ASSERT_TRUE(resolution.committedSemanticResolution);
  ASSERT_EQ(resolution.witnesses.size(), 1u);
  const AlignmentSemanticResolutionWitness &witness =
      resolution.witnesses.front();
  EXPECT_EQ(
      witness.resolutionKind,
      AlignmentSemanticResolutionKind::StructureRespectingTerminalRecovery);
  EXPECT_EQ(witness.candidateDomain,
            AlignmentSemanticCandidateDomain::StructureRepairCandidates);
  EXPECT_EQ(witness.structureRepairScope,
            StructureRepairScope::AllPreferredSubRectangles);
  EXPECT_EQ(witness.variedSubRectangleCount, 1u);
  EXPECT_EQ(witness.candidateCount, 1u);
  EXPECT_EQ(witness.acceptedCandidateCount, 1u);
  EXPECT_EQ(witness.equivalenceKey, "output of 3,4,5,6");
  ASSERT_FALSE(witness.anchorEvidence.empty());
  for (const AlignmentSemanticAnchorEvidence &evidence : witness.anchorEvidence)
    EXPECT_EQ(evidence.basis,
              AlignmentSemanticAnchorBasis::
                  StructureRespectingTerminalRecoveryRepresentative);

  AlignmentSelectionOverride selected;
  selected.selectedMap = resolution.selectedMap;
  EXPECT_EQ(unmatchedIn(selected, 0, 11), "3,4,5,6");
  EXPECT_EQ(resolution.structurePreservingTieRanges,
            std::vector<TokenRange>{harness.WindowARange(0)});

  // The committed map was realized with its window's ties settled for
  // structure, as production will realize it.
  const bool realizedWithTieRange = llvm::any_of(
      harness.realized, [&](const AlignmentSelectionOverride &selection) {
        return unmatchedIn(selection, 0, 11) == "3,4,5,6" &&
               selection.structurePreservingTieRanges ==
                   std::vector<TokenRange>{harness.WindowARange(0)};
      });
  EXPECT_TRUE(realizedWithTieRange);

  ASSERT_TRUE(resolution.replayReference.has_value());
  EXPECT_EQ(resolution.replayReference->witnessId, witness.witnessId);
  EXPECT_EQ(resolution.replayReference->concreteOutputKey, "output of 3,4,5,6");
}

TEST(RefoldAlignmentSemanticResolver,
     AdmissibleDeclinePreventsStructureRecovery) {
  ResolverHarness harness = structureShape();
  harness.simulate = distinctOutputsUnless(accepted("declined output"));

  const Resolution resolution = harness.Resolve();
  EXPECT_FALSE(resolution.committedSemanticResolution);
  EXPECT_TRUE(resolution.witnesses.empty());
  EXPECT_FALSE(resolution.replayReference.has_value());
  EXPECT_EQ(resolution.selectedMap, harness.Core().forcedMap);
}

/// The structure shape with only the second `#define` protected, so the repair
/// keeps the two runs that do not straddle it, [2,6) and [3,7).  Their
/// realizations with the structure tie range are answered by \p first and
/// \p second; every other realization as in `distinctOutputsUnless()`.
ResolverHarness twoPreferredCandidates(Simulation first, Simulation second) {
  ResolverHarness harness(StructureShapeA, StructureShapeB);
  harness.protectedBoundaries = {7};
  harness.simulate = [first, second,
                      fallback = distinctOutputsUnless(terminal())](
                         const AlignmentSelectionOverride &selection) {
    if (!selection.structurePreservingTieRanges.empty()) {
      const std::string run = unmatchedIn(selection, 0, 11);
      if (run == "2,3,4,5")
        return first;
      if (run == "3,4,5,6")
        return second;
    }
    return fallback(selection);
  };
  return harness;
}

TEST(RefoldAlignmentSemanticResolver,
     PreferredCandidatesSharingOneOutputCommit) {
  ResolverHarness harness =
      twoPreferredCandidates(accepted("shared"), accepted("shared"));
  const Resolution resolution = harness.Resolve();
  ASSERT_TRUE(resolution.committedSemanticResolution);
  ASSERT_EQ(resolution.witnesses.size(), 1u);
  EXPECT_EQ(resolution.witnesses.front().candidateCount, 2u);
  EXPECT_EQ(resolution.witnesses.front().acceptedCandidateCount, 2u);
  EXPECT_EQ(resolution.witnesses.front().equivalenceKey, "shared");
}

TEST(RefoldAlignmentSemanticResolver,
     PreferredCandidatesWithTwoOutputsDecline) {
  ResolverHarness harness =
      twoPreferredCandidates(accepted("one"), accepted("another"));
  const Resolution resolution = harness.Resolve();
  EXPECT_FALSE(resolution.committedSemanticResolution);
  EXPECT_EQ(resolution.selectedMap, harness.Core().forcedMap);
}

TEST(RefoldAlignmentSemanticResolver,
     TerminalPreferredCandidateIsNotARepresentative) {
  ResolverHarness harness =
      twoPreferredCandidates(terminal(), accepted("only"));
  const Resolution resolution = harness.Resolve();
  ASSERT_TRUE(resolution.committedSemanticResolution);
  ASSERT_EQ(resolution.witnesses.size(), 1u);
  EXPECT_EQ(resolution.witnesses.front().candidateCount, 2u);
  EXPECT_EQ(resolution.witnesses.front().acceptedCandidateCount, 1u);
  EXPECT_EQ(resolution.witnesses.front().equivalenceKey, "only");
}

TEST(RefoldAlignmentSemanticResolver,
     ProofIncompletePreferredCandidateDeclines) {
  ResolverHarness harness =
      twoPreferredCandidates(proofIncomplete(), accepted("only"));
  const Resolution resolution = harness.Resolve();
  EXPECT_FALSE(resolution.committedSemanticResolution);
  EXPECT_EQ(resolution.selectedMap, harness.Core().forcedMap);
}

/// `a x..x b` against `a x..x b` with \p aRepeats and \p bRepeats copies of
/// `x`, where only the map matching the last \p bRepeats copies is
/// line-aligned.  Every realization but the decline's is accepted with its own
/// output.
ResolverHarness repeatedTokenRun(size_t aRepeats, size_t bRepeats) {
  std::vector<llvm::StringRef> a(1, "a"), b(1, "a");
  a.insert(a.end(), aRepeats, "x");
  b.insert(b.end(), bRepeats, "x");
  a.push_back("b");
  b.push_back("b");
  ResolverHarness harness(a, b);
  const uint64_t deleted = aRepeats - bRepeats;
  harness.beginsLine = {1};
  harness.endsLine = {deleted};
  harness.simulate =
      [aTokens = a.size()](const AlignmentSelectionOverride &selection) {
        // The decline leaves every `x` unmatched.
        const std::string run = unmatchedIn(selection, 0, aTokens);
        std::vector<std::string> everyRepeat;
        for (uint64_t aToken = 1; aToken + 1 < aTokens; ++aToken)
          everyRepeat.push_back(std::to_string(aToken));
        if (run == llvm::join(everyRepeat, ","))
          return terminal();
        return accepted("output of " + run);
      };
  return harness;
}

TEST(RefoldAlignmentSemanticResolver, SubRectangleWithinTheRepairBoundCommits) {
  // 14 choose 7 = 3432 optimal maps, within the bound of 4096.
  ResolverHarness harness = repeatedTokenRun(14, 7);
  const Resolution resolution = harness.Resolve();
  ASSERT_TRUE(resolution.committedSemanticResolution);
  ASSERT_EQ(resolution.witnesses.size(), 1u);
  EXPECT_EQ(
      resolution.witnesses.front().resolutionKind,
      AlignmentSemanticResolutionKind::StructureRespectingTerminalRecovery);
  EXPECT_EQ(resolution.witnesses.front().equivalenceKey,
            "output of 1,2,3,4,5,6,7");
}

TEST(RefoldAlignmentSemanticResolver, SubRectangleOverTheRepairBoundDeclines) {
  // 15 choose 7 = 6435 optimal maps, over the bound of 4096.
  ResolverHarness harness = repeatedTokenRun(15, 8);
  const Resolution resolution = harness.Resolve();
  EXPECT_FALSE(resolution.committedSemanticResolution);
  EXPECT_EQ(resolution.selectedMap, harness.Core().forcedMap);
}

/// Nine sub-rectangles `s x x x` against `s x x`, closed by a final `s`.  Each
/// keeps the two maps deleting its first or second `x`, whose one-token runs
/// are line-aligned, so the repair's combinations number 2^9, over the
/// realization budget.  The decline's terminal requests name \p requests; the
/// candidates varying only sub-rectangle 1 share one output.
ResolverHarness
nineSubRectangles(std::vector<std::optional<TokenRange>> requests) {
  std::vector<llvm::StringRef> a, b;
  std::set<uint64_t> lineAligned;
  for (size_t rectangle = 0; rectangle < 9; ++rectangle) {
    a.push_back("s");
    b.push_back("s");
    lineAligned.insert(a.size());
    lineAligned.insert(a.size() + 1);
    a.insert(a.end(), 3, "x");
    b.insert(b.end(), 2, "x");
  }
  a.push_back("s");
  b.push_back("s");
  ResolverHarness harness(a, b);
  harness.beginsLine = lineAligned;
  harness.endsLine = lineAligned;
  // Only the decline is realized without the structure tie range.
  harness.simulate = [requests](const AlignmentSelectionOverride &selection) {
    if (selection.structurePreservingTieRanges.empty())
      return terminal(requests);
    return accepted("narrowed output");
  };
  return harness;
}

TEST(RefoldAlignmentSemanticResolver, TerminalRequestNarrowsTheRepair) {
  ResolverHarness harness = nineSubRectangles({TokenRange{5, 6}});
  const Resolution resolution = harness.Resolve();
  ASSERT_TRUE(resolution.committedSemanticResolution);
  ASSERT_EQ(resolution.witnesses.size(), 1u);
  const AlignmentSemanticResolutionWitness &witness =
      resolution.witnesses.front();
  EXPECT_EQ(witness.structureRepairScope,
            StructureRepairScope::TerminalRequestIntersectingSubRectangles);
  EXPECT_EQ(witness.variedSubRectangleCount, 1u);
  EXPECT_EQ(witness.candidateCount, 2u);
}

TEST(RefoldAlignmentSemanticResolver, UnlocatedTerminalRequestDeclines) {
  // The located request alone narrows the repair to sub-rectangle 1; the
  // unlocated one could be failing anywhere, so nothing may be narrowed away.
  ResolverHarness harness = nineSubRectangles({TokenRange{5, 6}, std::nullopt});
  EXPECT_FALSE(harness.Resolve().committedSemanticResolution);
}

TEST(RefoldAlignmentSemanticResolver, NonintersectingTerminalRequestDeclines) {
  // [0,1) is the first forced `s`, which no sub-rectangle varies.
  ResolverHarness harness = nineSubRectangles({TokenRange{0, 1}});
  EXPECT_FALSE(harness.Resolve().committedSemanticResolution);
}

/// `p x x q` against `p x q`: two optimal maps that the fake simulations below
/// give one output, so the observational-irrelevance rule commits it.
const std::vector<llvm::StringRef> EquivalenceShapeA = {"p", "x", "x", "q"};
const std::vector<llvm::StringRef> EquivalenceShapeB = {"p", "x", "q"};

/// Answer a selection over the structure shape at A `[0, 11)` followed by the
/// equivalence shape at A `[11, 15)`.
///
/// The structure shape's decline requests terminal fallback.  Otherwise the
/// output depends on the structure shape's unmatched run, on how many tokens
/// the equivalence shape leaves unmatched but not which, and on the tie ranges
/// realized.
Simulation
structureThenEquivalenceOutput(const AlignmentSelectionOverride &selection) {
  const std::string run = unmatchedIn(selection, 0, 11);
  if (run == "2,3,4,5,6,7,8")
    return terminal();
  uint64_t equivalenceUnmatched = 0;
  for (uint64_t aToken = 11; aToken < 15; ++aToken)
    equivalenceUnmatched += selection.selectedMap[aToken] < 0;
  return accepted("structure " + run + "; equivalence unmatched " +
                  std::to_string(equivalenceUnmatched) + "; ties " +
                  tieRangesOf(selection));
}

TEST(RefoldAlignmentSemanticResolver, LaterCommitIsTheReplayReference) {
  // Window 0 commits a structure recovery and window 1, after it, a
  // complete-ground-set equivalence.  The seam at A 11 lies before the unique
  // `p`.
  //
  // The reverse order cannot commit both: every candidate of the earlier
  // window leaves the later one at its core-forced anchors, whose realization
  // requests terminal fallback, so the earlier window has nothing to accept.
  std::vector<llvm::StringRef> a = StructureShapeA, b = StructureShapeB;
  a.insert(a.end(), EquivalenceShapeA.begin(), EquivalenceShapeA.end());
  b.insert(b.end(), EquivalenceShapeB.begin(), EquivalenceShapeB.end());
  ResolverHarness harness(a, b, /*candidateABoundaries=*/{11});
  ASSERT_EQ(harness.Core().certificationWindows.size(), 2u);
  harness.protectedBoundaries = {3, 7};
  harness.simulate = structureThenEquivalenceOutput;

  const Resolution resolution = harness.Resolve();
  ASSERT_TRUE(resolution.committedSemanticResolution);
  ASSERT_EQ(resolution.witnesses.size(), 2u);
  const AlignmentSemanticResolutionWitness &recovery = resolution.witnesses[0];
  const AlignmentSemanticResolutionWitness &equivalence =
      resolution.witnesses[1];
  EXPECT_EQ(recovery.windowIndex, 0u);
  EXPECT_EQ(
      recovery.resolutionKind,
      AlignmentSemanticResolutionKind::StructureRespectingTerminalRecovery);
  EXPECT_EQ(equivalence.windowIndex, 1u);
  EXPECT_EQ(equivalence.resolutionKind,
            AlignmentSemanticResolutionKind::CompleteGroundSetEquivalence);
  EXPECT_FALSE(equivalence.structureRepairScope.has_value());
  EXPECT_EQ(equivalence.variedSubRectangleCount, 0u);

  // Only the structure recovery's own window settles ties for structure, and
  // every evidence record lies in the window of the witness holding it.
  const TokenRange structureWindow = harness.WindowARange(0);
  EXPECT_EQ(resolution.structurePreservingTieRanges,
            std::vector<TokenRange>{structureWindow});
  for (const AlignmentSemanticResolutionWitness &witness : resolution.witnesses)
    for (const AlignmentSemanticAnchorEvidence &evidence :
         witness.anchorEvidence) {
      EXPECT_LE(witness.aBegin, evidence.aToken);
      EXPECT_LT(evidence.aToken, witness.aEnd);
    }

  // The later window was realized with the earlier one's tie range, so its
  // simulation alone realized the final map under the final tie ranges.
  AlignmentSelectionOverride finalSelection;
  finalSelection.selectedMap = resolution.selectedMap;
  finalSelection.structurePreservingTieRanges = {structureWindow};
  const std::string finalOutput = structureThenEquivalenceOutput(finalSelection)
                                      .concreteOutputEquivalenceKey;
  ASSERT_TRUE(resolution.replayReference.has_value());
  EXPECT_EQ(resolution.replayReference->witnessId, equivalence.witnessId);
  EXPECT_EQ(resolution.replayReference->concreteOutputKey, finalOutput);

  // The earlier window's own key was realized with the later window still at
  // its core-forced anchors, so it does not predict the final output.
  EXPECT_NE(recovery.equivalenceKey, finalOutput);
}

} // namespace

//===--- RefoldLegacyAlignmentDiagnostic.cpp ------------------------------===//
//
// Boundary proposal reconstructed from the pre-semantic-resolver policy.
//
// This service reconstructs the last regression-passing pre-semantic-resolver
// partial map from the exact all-optimal oracle. The historical ranks are
// proposal-only: the semantic resolver must independently prove every non-
// forced anchor by counterfactual planning or realized-source equivalence
// before it can become production authority.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldLegacyAlignmentDiagnostic.h"

#include <algorithm>
#include <limits>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

bool hasProvenanceId(uint64_t value) {
  return value != diffutils::LcsAGapProvenance::noId;
}

uint64_t aBoundaryRetentionRank(
    ArrayRef<diffutils::LcsAGapProvenance> profiles, uint64_t gap) {
  if (gap >= profiles.size())
    return 0;

  const diffutils::LcsAGapProvenance &profile =
      profiles[static_cast<size_t>(gap)];
  uint64_t rank = profile.ownerDepth;
  rank += static_cast<uint64_t>(profile.includeDepth) * 8;
  rank += static_cast<uint64_t>(profile.conditionalDepth) * 8;
  rank += static_cast<uint64_t>(profile.macroDepth) * 8;
  rank += hasProvenanceId(profile.leftIncludeId) ? 1 : 0;
  rank += hasProvenanceId(profile.rightIncludeId) ? 1 : 0;
  rank += hasProvenanceId(profile.lcaIncludeId) ? 1 : 0;
  rank += hasProvenanceId(profile.leftCondGroupId) ? 1 : 0;
  rank += hasProvenanceId(profile.leftCondArmId) ? 1 : 0;
  rank += hasProvenanceId(profile.rightCondGroupId) ? 1 : 0;
  rank += hasProvenanceId(profile.rightCondArmId) ? 1 : 0;
  rank += hasProvenanceId(profile.leftMacroRootId) ? 1 : 0;
  rank += hasProvenanceId(profile.leftMacroLeafId) ? 1 : 0;
  rank += hasProvenanceId(profile.rightMacroRootId) ? 1 : 0;
  rank += hasProvenanceId(profile.rightMacroLeafId) ? 1 : 0;
  rank += profile.leftMacroRoleMask ? 1 : 0;
  rank += profile.rightMacroRoleMask ? 1 : 0;
  return rank;
}

bool sameBLineShape(const diffutils::LcsBGapProvenance &lhs,
                    const diffutils::LcsBGapProvenance &rhs) {
  return lhs.hasLeftToken == rhs.hasLeftToken &&
         lhs.hasRightToken == rhs.hasRightToken &&
         lhs.gapContainsNewline == rhs.gapContainsNewline &&
         lhs.gapContainsOnlyWs == rhs.gapContainsOnlyWs &&
         lhs.gapAtLineStart == rhs.gapAtLineStart &&
         lhs.gapAtLineEnd == rhs.gapAtLineEnd &&
         lhs.leftTokenStartsLine == rhs.leftTokenStartsLine &&
         lhs.leftTokenEndsLine == rhs.leftTokenEndsLine &&
         lhs.rightTokenStartsLine == rhs.rightTokenStartsLine &&
         lhs.rightTokenEndsLine == rhs.rightTokenEndsLine;
}

uint64_t bGapSurfaceRank(
    ArrayRef<diffutils::LcsBGapProvenance> profiles, uint64_t gap) {
  if (gap >= profiles.size())
    return 0;

  const diffutils::LcsBGapProvenance &profile =
      profiles[static_cast<size_t>(gap)];
  uint64_t rank = 0;
  rank += profile.hasLeftToken ? 1 : 0;
  rank += profile.hasRightToken ? 1 : 0;
  rank += profile.gapContainsOnlyWs ? 1 : 0;
  rank += !profile.gapContainsNewline ? 4 : 0;
  rank += !profile.gapAtLineStart ? 2 : 0;
  rank += !profile.gapAtLineEnd ? 2 : 0;
  rank += profile.leftTokenEndsLine == profile.rightTokenEndsLine ? 1 : 0;
  rank += profile.leftTokenStartsLine == profile.rightTokenStartsLine ? 1 : 0;
  return rank;
}

uint64_t bPairSurfaceRank(
    ArrayRef<diffutils::LcsBGapProvenance> profiles, uint64_t bStart,
    uint64_t bEnd) {
  uint64_t rank =
      bGapSurfaceRank(profiles, bStart) + bGapSurfaceRank(profiles, bEnd);
  if (bStart < profiles.size() && bEnd < profiles.size() &&
      sameBLineShape(profiles[static_cast<size_t>(bStart)],
                     profiles[static_cast<size_t>(bEnd)]))
    rank += 8;
  return rank;
}

size_t suppressOrderConflictingAnchors(std::vector<int64_t> &map) {
  const size_t count = map.size();
  if (count == 0)
    return 0;

  std::vector<int64_t> maxLeft(count, -1);
  int64_t leftMax = -1;
  for (size_t index = 0; index < count; ++index) {
    maxLeft[index] = leftMax;
    if (map[index] >= 0)
      leftMax = std::max(leftMax, map[index]);
  }

  std::vector<int64_t> minRight(
      count, std::numeric_limits<int64_t>::max());
  int64_t rightMin = std::numeric_limits<int64_t>::max();
  for (size_t index = count; index-- > 0;) {
    minRight[index] = rightMin;
    if (map[index] >= 0)
      rightMin = std::min(rightMin, map[index]);
  }

  size_t suppressed = 0;
  for (size_t index = 0; index < count; ++index) {
    const int64_t mapped = map[index];
    if (mapped < 0)
      continue;
    if (maxLeft[index] >= mapped || minRight[index] <= mapped) {
      map[index] = -1;
      ++suppressed;
    }
  }
  return suppressed;
}

bool mapIsMonotone(ArrayRef<int64_t> map, size_t bTokenCount) {
  int64_t previous = -1;
  for (int64_t mapped : map) {
    if (mapped < 0)
      continue;
    if (static_cast<uint64_t>(mapped) >= bTokenCount || mapped <= previous)
      return false;
    previous = mapped;
  }
  return true;
}

bool mapLexemesAgree(ArrayRef<int64_t> map, ArrayRef<StringRef> aLexemes,
                     ArrayRef<StringRef> bLexemes) {
  if (map.size() != aLexemes.size())
    return false;
  for (size_t aToken = 0; aToken < map.size(); ++aToken) {
    const int64_t bToken = map[aToken];
    if (bToken < 0)
      continue;
    if (static_cast<size_t>(bToken) >= bLexemes.size() ||
        aLexemes[aToken] != bLexemes[static_cast<size_t>(bToken)])
      return false;
  }
  return true;
}

bool addObjectiveChecked(diffutils::LcsObjective &total,
                         const diffutils::LcsObjective &part) {
  if (total.matchedTokenCount >
          std::numeric_limits<uint32_t>::max() - part.matchedTokenCount ||
      total.ownerDepthCost >
          std::numeric_limits<uint64_t>::max() - part.ownerDepthCost)
    return false;
  total.matchedTokenCount += part.matchedTokenCount;
  total.ownerDepthCost += part.ownerDepthCost;
  return true;
}

bool mapCanOccurOnOneOptimalPath(
    ArrayRef<int64_t> map, const diffutils::CertifiedLcsResult &alignment,
    std::string &failure) {
  if (!alignment.HasCompleteSemanticOracleForWindow(/*windowIndex=*/0)) {
    failure = "core all-optimal oracle is incomplete";
    return false;
  }

  uint64_t aBegin = 0;
  uint64_t bBegin = 0;
  diffutils::LcsObjective conditioned;

  for (size_t aToken = 0; aToken < map.size(); ++aToken) {
    const int64_t mapped = map[aToken];
    if (mapped < 0)
      continue;
    const uint64_t bToken = static_cast<uint64_t>(mapped);
    if (aToken < aBegin || bToken < bBegin ||
        !alignment.oracle.PairOccursOnOptimalPath(aToken, bToken)) {
      failure = "selected anchor is not individually core-optimal";
      return false;
    }

    const diffutils::LcsObjective prefix =
        alignment.oracle.ObjectiveForWindow(aBegin, aToken, bBegin, bToken);
    if (!addObjectiveChecked(conditioned, prefix) ||
        conditioned.matchedTokenCount ==
            std::numeric_limits<uint32_t>::max()) {
      failure = "conditioned objective overflowed";
      return false;
    }
    // The selected anchor contributes one additional match after the
    // conditioned prefix.  Check before incrementing so the evidence-only
    // diagnostic cannot wrap and accidentally report joint optimality.
    ++conditioned.matchedTokenCount;
    aBegin = aToken + 1;
    bBegin = bToken + 1;
  }

  const diffutils::LcsObjective suffix = alignment.oracle.ObjectiveForWindow(
      aBegin, alignment.oracle.GetATokenCount(), bBegin,
      alignment.oracle.GetBTokenCount());
  if (!addObjectiveChecked(conditioned, suffix)) {
    failure = "conditioned suffix objective overflowed";
    return false;
  }

  if (conditioned.matchedTokenCount !=
          alignment.globalObjective.matchedTokenCount ||
      conditioned.ownerDepthCost != alignment.globalObjective.ownerDepthCost) {
    failure = "selected anchors cannot coexist on one globally optimal path";
    return false;
  }
  return true;
}

struct BoundaryPureCandidate {
  uint64_t left = 0;
  uint64_t right = 0;
  uint64_t balance = 0;
  uint64_t boundaryRank = 0;
  uint64_t bPairRank = 0;
};

struct SuffixInsertionCandidate {
  uint64_t left = 0;
  uint64_t right = 0;
  uint64_t anchorA = 0;
  uint64_t anchorB = 0;
  uint64_t boundaryRank = 0;
  uint64_t bPairRank = 0;
  uint64_t restoredAnchors = 0;
};

bool betterBoundaryPureCandidate(const BoundaryPureCandidate &candidate,
                                 const BoundaryPureCandidate &best) {
  if (candidate.balance != best.balance)
    return candidate.balance < best.balance;
  if (candidate.boundaryRank != best.boundaryRank)
    return candidate.boundaryRank > best.boundaryRank;
  return candidate.bPairRank > best.bPairRank;
}

bool betterSuffixInsertionCandidate(
    const SuffixInsertionCandidate &candidate,
    const SuffixInsertionCandidate &best) {
  if (candidate.boundaryRank != best.boundaryRank)
    return candidate.boundaryRank > best.boundaryRank;
  if (candidate.bPairRank != best.bPairRank)
    return candidate.bPairRank > best.bPairRank;
  if (candidate.restoredAnchors != best.restoredAnchors)
    return candidate.restoredAnchors > best.restoredAnchors;
  if (candidate.right != best.right)
    return candidate.right > best.right;
  return candidate.left > best.left;
}

} // namespace

StringRef toString(LegacyAlignmentAnchorOrigin origin) {
  switch (origin) {
  case LegacyAlignmentAnchorOrigin::None:
    return "none";
  case LegacyAlignmentAnchorOrigin::CoreForced:
    return "core-forced";
  case LegacyAlignmentAnchorOrigin::BidirectionallyUniqueAdmissible:
    return "bidirectionally-unique-admissible";
  case LegacyAlignmentAnchorOrigin::BoundaryPureRestoration:
    return "boundary-pure-restoration";
  case LegacyAlignmentAnchorOrigin::SuffixInsertionRestoration:
    return "suffix-insertion-restoration";
  }
  llvm_unreachable("invalid legacy alignment anchor origin");
}

LegacyAlignmentDiagnosticResult reconstructLegacyBoundaryProposal(
    ArrayRef<StringRef> aLexemes, ArrayRef<StringRef> bLexemes,
    ArrayRef<diffutils::LcsAGapProvenance> aGapProvenance,
    ArrayRef<diffutils::LcsBGapProvenance> bGapProvenance,
    const diffutils::CertifiedLcsResult &coreAlignment) {
  LegacyAlignmentDiagnosticResult result;
  const size_t aCount = aLexemes.size();
  const size_t bCount = bLexemes.size();
  result.selectedMap.assign(aCount, -1);
  result.anchorOrigins.assign(aCount, LegacyAlignmentAnchorOrigin::None);

  if (!coreAlignment.HasCompleteSemanticOracleForWindow(
          /*windowIndex=*/0)) {
    result.constructionFailure = "core all-optimal certification is incomplete";
    return result;
  }
  if (coreAlignment.forcedMap.size() != aCount) {
    result.constructionFailure = "forced-map length does not match A tokens";
    return result;
  }
  if (aGapProvenance.size() != aCount + 1) {
    result.constructionFailure = "A-gap provenance length is incomplete";
    return result;
  }
  if (bGapProvenance.size() != bCount + 1) {
    result.constructionFailure = "B-gap provenance length is incomplete";
    return result;
  }

  std::vector<uint32_t> aPartnerCount(aCount, 0);
  std::vector<uint32_t> bPartnerCount(bCount, 0);
  std::vector<int64_t> firstBPartner(aCount, -1);
  for (size_t aToken = 0; aToken < aCount; ++aToken) {
    for (size_t bToken = 0; bToken < bCount; ++bToken) {
      if (aLexemes[aToken] != bLexemes[bToken] ||
          !coreAlignment.oracle.PairOccursOnOptimalPath(aToken, bToken))
        continue;
      if (aPartnerCount[aToken] == 0)
        firstBPartner[aToken] = static_cast<int64_t>(bToken);
      ++aPartnerCount[aToken];
      ++bPartnerCount[bToken];
    }
  }

  for (size_t aToken = 0; aToken < aCount; ++aToken) {
    if (aPartnerCount[aToken] != 1)
      continue;
    const int64_t bToken = firstBPartner[aToken];
    if (bToken < 0 || static_cast<size_t>(bToken) >= bCount ||
        bPartnerCount[static_cast<size_t>(bToken)] != 1)
      continue;
    result.selectedMap[aToken] = bToken;
    result.anchorOrigins[aToken] =
        LegacyAlignmentAnchorOrigin::BidirectionallyUniqueAdmissible;
  }

  const std::vector<int64_t> seedBeforeSuppression = result.selectedMap;
  result.crossingAnchorSuppressions =
      suppressOrderConflictingAnchors(result.selectedMap);
  for (size_t aToken = 0; aToken < aCount; ++aToken) {
    if (seedBeforeSuppression[aToken] >= 0 && result.selectedMap[aToken] < 0)
      result.anchorOrigins[aToken] = LegacyAlignmentAnchorOrigin::None;
  }

  auto canUseAmbiguousEdgeAnchor = [&](uint64_t aToken,
                                       uint64_t bToken) -> bool {
    if (aToken >= aCount || bToken >= bCount ||
        aLexemes[static_cast<size_t>(aToken)] !=
            bLexemes[static_cast<size_t>(bToken)] ||
        !coreAlignment.oracle.PairOccursOnOptimalPath(aToken, bToken))
      return false;
    return !(aPartnerCount[static_cast<size_t>(aToken)] == 1 &&
             bPartnerCount[static_cast<size_t>(bToken)] == 1);
  };

  const std::vector<diffutils::Hunk> seedHunks = diffutils::hunksFromMap(
      result.selectedMap, coreAlignment.certifiedBoundaries, aCount, bCount);
  for (const diffutils::Hunk &seedHunk : seedHunks) {
    const uint64_t aWidth = seedHunk.aEnd - seedHunk.aStart;
    const uint64_t bWidth = seedHunk.bEnd - seedHunk.bStart;
    const uint64_t maxSharedWidth = std::min(aWidth, bWidth);
    if (maxSharedWidth == 0)
      continue;

    uint64_t maxPrefix = 0;
    while (maxPrefix < maxSharedWidth &&
           canUseAmbiguousEdgeAnchor(seedHunk.aStart + maxPrefix,
                                     seedHunk.bStart + maxPrefix))
      ++maxPrefix;

    uint64_t maxSuffix = 0;
    while (maxSuffix < maxSharedWidth &&
           canUseAmbiguousEdgeAnchor(seedHunk.aEnd - maxSuffix - 1,
                                     seedHunk.bEnd - maxSuffix - 1))
      ++maxSuffix;

    bool haveBest = false;
    BoundaryPureCandidate best;
    size_t bestCount = 0;
    for (uint64_t left = 0; left <= maxPrefix; ++left) {
      for (uint64_t right = 0; right <= maxSuffix; ++right) {
        if (left + right > maxSharedWidth)
          continue;
        const uint64_t aStart = seedHunk.aStart + left;
        const uint64_t aEnd = seedHunk.aEnd - right;
        const uint64_t bStart = seedHunk.bStart + left;
        const uint64_t bEnd = seedHunk.bEnd - right;
        if (aStart > aEnd || bStart > bEnd ||
            !(aStart == aEnd && bStart < bEnd))
          continue;

        const BoundaryPureCandidate candidate{
            left,
            right,
            left > right ? left - right : right - left,
            aBoundaryRetentionRank(aGapProvenance, aStart),
            bPairSurfaceRank(bGapProvenance, bStart, bEnd)};
        if (!haveBest || betterBoundaryPureCandidate(candidate, best)) {
          best = candidate;
          haveBest = true;
          bestCount = 1;
        } else if (candidate.balance == best.balance &&
                   candidate.boundaryRank == best.boundaryRank &&
                   candidate.bPairRank == best.bPairRank) {
          ++bestCount;
        }
      }
    }

    if (!haveBest) {
      bool haveSuffixBest = false;
      SuffixInsertionCandidate suffixBest;
      size_t suffixBestCount = 0;
      for (uint64_t right = 1; right <= maxSuffix; ++right) {
        const uint64_t aPureGap = seedHunk.aEnd - right;
        const uint64_t bPureEnd = seedHunk.bEnd - right;
        if (aPureGap == 0 || aPureGap <= seedHunk.aStart ||
            bPureEnd <= seedHunk.bStart)
          continue;

        const uint64_t anchorA = aPureGap - 1;
        for (uint64_t left = 0; left <= maxPrefix; ++left) {
          if (left + right + 1 > maxSharedWidth)
            continue;
          const uint64_t leftA = seedHunk.aStart + left;
          const uint64_t leftB = seedHunk.bStart + left;
          if (anchorA < leftA)
            continue;

          for (uint64_t anchorB = leftB; anchorB < bPureEnd; ++anchorB) {
            if (!canUseAmbiguousEdgeAnchor(anchorA, anchorB))
              continue;
            const uint64_t insertionBegin = anchorB + 1;
            if (insertionBegin >= bPureEnd)
              continue;

            const SuffixInsertionCandidate candidate{
                left,
                right,
                anchorA,
                anchorB,
                aBoundaryRetentionRank(aGapProvenance, aPureGap),
                bPairSurfaceRank(bGapProvenance, insertionBegin, bPureEnd),
                left + right + 1};
            if (!haveSuffixBest ||
                betterSuffixInsertionCandidate(candidate, suffixBest)) {
              suffixBest = candidate;
              haveSuffixBest = true;
              suffixBestCount = 1;
            } else if (
                candidate.boundaryRank == suffixBest.boundaryRank &&
                candidate.bPairRank == suffixBest.bPairRank &&
                candidate.restoredAnchors == suffixBest.restoredAnchors &&
                candidate.right == suffixBest.right &&
                candidate.left == suffixBest.left) {
              ++suffixBestCount;
            }
          }
        }
      }

      if (!haveSuffixBest || suffixBestCount != 1)
        continue;

      for (uint64_t offset = 0; offset < suffixBest.left; ++offset) {
        const size_t aToken =
            static_cast<size_t>(seedHunk.aStart + offset);
        result.selectedMap[aToken] =
            static_cast<int64_t>(seedHunk.bStart + offset);
        result.anchorOrigins[aToken] =
            LegacyAlignmentAnchorOrigin::SuffixInsertionRestoration;
      }
      result.selectedMap[static_cast<size_t>(suffixBest.anchorA)] =
          static_cast<int64_t>(suffixBest.anchorB);
      result.anchorOrigins[static_cast<size_t>(suffixBest.anchorA)] =
          LegacyAlignmentAnchorOrigin::SuffixInsertionRestoration;
      for (uint64_t offset = 0; offset < suffixBest.right; ++offset) {
        const size_t aToken = static_cast<size_t>(
            seedHunk.aEnd - suffixBest.right + offset);
        result.selectedMap[aToken] = static_cast<int64_t>(
            seedHunk.bEnd - suffixBest.right + offset);
        result.anchorOrigins[aToken] =
            LegacyAlignmentAnchorOrigin::SuffixInsertionRestoration;
      }
      continue;
    }

    if (bestCount != 1)
      continue;

    for (uint64_t offset = 0; offset < best.left; ++offset) {
      const size_t aToken =
          static_cast<size_t>(seedHunk.aStart + offset);
      result.selectedMap[aToken] =
          static_cast<int64_t>(seedHunk.bStart + offset);
      result.anchorOrigins[aToken] =
          LegacyAlignmentAnchorOrigin::BoundaryPureRestoration;
    }
    for (uint64_t offset = 0; offset < best.right; ++offset) {
      const size_t aToken = static_cast<size_t>(
          seedHunk.aEnd - best.right + offset);
      result.selectedMap[aToken] = static_cast<int64_t>(
          seedHunk.bEnd - best.right + offset);
      result.anchorOrigins[aToken] =
          LegacyAlignmentAnchorOrigin::BoundaryPureRestoration;
    }
  }

  int64_t previousB = -1;
  bool crossed = false;
  for (int64_t mapped : result.selectedMap) {
    if (mapped < 0)
      continue;
    if (mapped <= previousB) {
      crossed = true;
      break;
    }
    previousB = mapped;
  }
  if (crossed) {
    result.selectedMap = seedBeforeSuppression;
    suppressOrderConflictingAnchors(result.selectedMap);
    std::fill(result.anchorOrigins.begin(), result.anchorOrigins.end(),
              LegacyAlignmentAnchorOrigin::None);
    for (size_t aToken = 0; aToken < aCount; ++aToken) {
      if (result.selectedMap[aToken] >= 0)
        result.anchorOrigins[aToken] =
            LegacyAlignmentAnchorOrigin::BidirectionallyUniqueAdmissible;
    }
  }

  for (size_t aToken = 0; aToken < aCount; ++aToken) {
    if (result.selectedMap[aToken] < 0)
      continue;
    if (aToken < coreAlignment.forcedMap.size() &&
        coreAlignment.forcedMap[aToken] == result.selectedMap[aToken])
      result.anchorOrigins[aToken] = LegacyAlignmentAnchorOrigin::CoreForced;

    switch (result.anchorOrigins[aToken]) {
    case LegacyAlignmentAnchorOrigin::None:
      break;
    case LegacyAlignmentAnchorOrigin::CoreForced:
      ++result.coreForcedAnchorCount;
      break;
    case LegacyAlignmentAnchorOrigin::BidirectionallyUniqueAdmissible:
      ++result.uniquePartnerAnchorCount;
      break;
    case LegacyAlignmentAnchorOrigin::BoundaryPureRestoration:
      ++result.boundaryPureAnchorCount;
      break;
    case LegacyAlignmentAnchorOrigin::SuffixInsertionRestoration:
      ++result.suffixInsertionAnchorCount;
      break;
    }
  }

  result.monotone = mapIsMonotone(result.selectedMap, bCount);
  result.lexemesAgree =
      mapLexemesAgree(result.selectedMap, aLexemes, bLexemes);
  if (result.monotone && result.lexemesAgree) {
    result.jointlyCoreOptimal = mapCanOccurOnOneOptimalPath(
        result.selectedMap, coreAlignment, result.jointOptimalityFailure);
  } else {
    result.jointOptimalityFailure =
        !result.monotone ? "legacy shadow map is non-monotone"
                         : "legacy shadow map does not preserve token equality";
  }
  result.hunks = diffutils::hunksFromMap(
      result.selectedMap, coreAlignment.certifiedBoundaries, aCount, bCount);
  result.complete = true;
  return result;
}

} // namespace refold
} // namespace clang

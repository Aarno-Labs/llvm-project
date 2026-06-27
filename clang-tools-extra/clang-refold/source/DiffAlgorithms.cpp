//===--- DiffAlgorithms.cpp -------------------------------------*- C++ -*-===//
//
// Diff/Alignment utilities used by the refolding pipeline.
//
// Overview
// --------
// This component provides small, deterministic sequence algorithms that power
// refolding’s token alignment and edit extraction:
//
//   • LCS (Longest Common Subsequence) map A→B over arbitrary element types
//     (typically token spellings), including owner-aware and
//     provenance-certified variants used by the refolder.
//   • Hunk construction from an A→B alignment (contiguous edit regions).
//   • Myers O((N+M)*D) shortest edit script (SES) with linear-space reconstruc-
//     tion.
//
// Responsibilities
// ----------------
//   • lcsMapAB: compute a one-sided mapping from indices in A to matching
//     indices in B (or -1 if unmatched). The structured overload first solves
//     the owner-aware LCS objective and then suppresses/restores ambiguous
//     anchors using provenance certificates.
//   • hunksFromMap: convert an A→B map into ordered edit hunks between anchors.
//   • myersDiff / coalesce: produce SES steps (EQUAL/INSERT/DELETE) and merge
//     adjacent non-EQUAL runs into hunks.
//
// Determinism & Policy
// --------------------
//   • Algorithms are deterministic. The refolder-facing LCS overload avoids
//     lexical neighbor tie heuristics by keeping only certified anchors.
//   • Large-input guard: LCS switches to Hirschberg recursion (exact) when
//     the full DP table would exceed a configured cell budget.
//   • Utilities are side-effect free and operate on caller-owned sequences.
//
// Complexity
// ----------
//   • LCS:      time O(N*M); DP uses O(N*M) space, Hirschberg uses O(N+M).
//   • Myers:    expected time O((N+M)*D), space O(N+M).
//   • Hunking:  O(N) over the alignment/map.
//
// Public Surface
// --------------
//   • std::vector<int64_t> lcsMapAB(...):
//       A[i] -> B[j] (j >= 0) or -1; owner-aware/provenance-certified when
//       structured gap profiles are supplied.
//   • std::vector<Hunk> hunksFromMap(const std::vector<int>& map,
//                                    int nA, int nB):
//       contiguous edit regions between anchors, half-open indices.
//   • std::vector<Step> myersDiff(const Seq& A, const Seq& B):
//       shortest edit script (EQUAL/INSERT/DELETE).
//   • std::vector<Hunk> coalesce(const std::vector<Step>& steps):
//       merges non-EQUAL runs into larger hunks.
//
// Notes
// -----
//   • Indices are zero-based; ranges are half-open [lo, hi).
//   • Implementations avoid undefined behavior around size/overflow and use
//     consistent types for indices and DP storage.
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "core/RefoldLog.h"
#include "source/DiffAlgorithms.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace clang {
namespace refold {
namespace diffutils {

constexpr size_t MAX =
    static_cast<size_t>(std::numeric_limits<int64_t>::max());

namespace {
/// Return true when the exact full DP table would exceed the configured cell or
/// allocation budget and the caller must use the exact linear-space Hirschberg
/// path. The function name is historical; the fallback is not greedy.
bool shouldUseGreedyApproach(unsigned long long n, unsigned long long m,
                             unsigned long long maxCells) {
  if (n >= std::numeric_limits<unsigned long long>::max() - 1ULL ||
      m >= std::numeric_limits<unsigned long long>::max() - 1ULL) {
    REFOLD_LOG_WARN("lcs/map",
         "hirschberg fallback: (n+1) or (m+1) would overflow unsigned long long"
         " (n={0}, m={1})",
         n, m);
    return true; // (n+1) or (m+1) would overflow ULL anyway
  } else {
    const unsigned long long n1 = n + 1ULL;
    const unsigned long long m1 = m + 1ULL;

    // Product check via division to avoid overflow: n1 * m1 > maxCells ?
    const unsigned long long maxN1 = maxCells / m1; // m1 >= 1 always
    if (n1 > maxN1) {
      REFOLD_LOG_WARN(
          "lcs/map",
          "hirschberg fallback: DP cell budget exceeded: (n+1)*(m+1) > maxCells "
          "(n={0}, m={1}, n1={2}, m1={3}, maxCells={4}, maxAllowedN1ForM1={5})",
          n, m, n1, m1, maxCells, maxN1);
      return true;
    } else {
      // Safe to multiply here: n1 <= maxCells/m1 implies n1*m1 <= maxCells (no
      // ULL overflow).
      const unsigned long long cells = n1 * m1;

      // Allocation guard: cells * sizeof(unsigned) must fit in size_t
      const unsigned long long cellLimit = static_cast<unsigned long long>(
          std::numeric_limits<size_t>::max() / sizeof(unsigned));

      if (cells > cellLimit) {
        REFOLD_LOG_WARN("lcs/map",
             "hirschberg fallback: DP allocation would overflow size_t for "
             "unsigned table "
             "(cells={0} > size_t/sizeof(unsigned)={1}; n={2}, m={3})",
             cells, cellLimit, n, m);
        return true;
      }
    }
  }
  return false;
}

/// Compare structural-LCS candidates: longer subsequence first, then lower
/// owner-depth cost.
inline bool isCoreBetter(unsigned candLen, std::uint64_t candCost,
                         unsigned bestLen, std::uint64_t bestCost) {
  if (candLen != bestLen)
    return candLen > bestLen;
  return candCost < bestCost;
}

/// Return the width of a hunk on the A side.
static uint64_t hunkAWidth(const Hunk &h) { return h.aEnd - h.aStart; }
/// Return the width of a hunk on the B side.
static uint64_t hunkBWidth(const Hunk &h) { return h.bEnd - h.bStart; }

/// Add \p extra to \p base without allowing unsigned wraparound.
static bool addCostChecked(uint64_t base, uint32_t extra, uint64_t &out) {
  if (base > std::numeric_limits<uint64_t>::max() - extra)
    return false;
  out = base + extra;
  return true;
}

/// DP cell for the heuristic-free structural LCS objective.
///
/// This intentionally omits the old neighbor-coherence tie penalty. It models
/// only the proof-relevant core objective: maximize LCS length, then minimize
/// ownerDepthGap cost. The forward/suffix tables built from this cell certify
/// whether a token pair is admissible in any optimal core solution.
struct CoreLcsCell {
  uint32_t len = 0;
  uint64_t cost = 0;
};

/// Compute forward and suffix DP tables for the structural LCS core objective.
///
/// The suffix table uses the same transition costs as the forward table:
/// deleting A[i] pays ownerDepthGap[i + 1], and inserting B[j] at the current
/// A gap pays ownerDepthGap[i]. Keeping those costs identical makes the
/// admissibility check a real certificate for the same objective used to build
/// the production map.
static bool buildCoreLcsDpTables(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                                 ArrayRef<uint32_t> ownerDepthGap,
                                 unsigned long long maxCells,
                                 std::vector<CoreLcsCell> &forward,
                                 std::vector<CoreLcsCell> &suffix,
                                 size_t &stride) {
  const size_t n = a.size();
  const size_t m = b.size();
  const unsigned long long nu = static_cast<unsigned long long>(n);
  const unsigned long long mu = static_cast<unsigned long long>(m);

  // Refuse to build quadratic DP tables when the configured cell budget says
  // this input must fall back to the exact linear-space Hirschberg path.
  if (shouldUseGreedyApproach(nu, mu, maxCells))
    return false;

  stride = m + 1;
  const size_t cells = (n + 1) * (m + 1);
  forward.assign(cells, CoreLcsCell{});
  suffix.assign(cells, CoreLcsCell{});

  auto idx = [&](size_t i, size_t j) -> size_t { return i * stride + j; };
  auto better = [](const CoreLcsCell &cand, const CoreLcsCell &best) {
    return isCoreBetter(cand.len, cand.cost, best.len, best.cost);
  };

  // Forward DP over prefixes A[0..i) and B[0..j). Each cell stores the best
  // objective value reachable for that prefix pair: maximize kept length, then
  // minimize structural gap cost.
  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      if (i == 0 && j == 0)
        continue;

      CoreLcsCell best{0, std::numeric_limits<uint64_t>::max()};

      // Keep a matching token pair. Matching increases the LCS length and does
      // not pay an owner-depth gap cost.
      if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
        CoreLcsCell cand = forward[idx(i - 1, j - 1)];
        ++cand.len;
        if (better(cand, best))
          best = cand;
      }

      // Delete A[i - 1]. In prefix coordinates, this deletion crosses the gap
      // after that A token, represented by ownerDepthGap[i].
      if (i > 0) {
        CoreLcsCell cand = forward[idx(i - 1, j)];
        if (addCostChecked(cand.cost, ownerDepthGap[i], cand.cost) &&
            better(cand, best))
          best = cand;
      }

      // Insert B[j - 1] at the current A prefix boundary. This insertion is
      // charged to the same A gap ownerDepthGap[i].
      if (j > 0) {
        CoreLcsCell cand = forward[idx(i, j - 1)];
        if (addCostChecked(cand.cost, ownerDepthGap[i], cand.cost) &&
            better(cand, best))
          best = cand;
      }

      forward[idx(i, j)] = best;
    }
  }

  // Suffix DP over suffixes A[i..n) and B[j..m). This table answers the same
  // objective as `forward`, but from the opposite direction so later
  // admissibility checks can prove that a proposed split still lies on an
  // optimal full solution.
  for (size_t ii = n + 1; ii > 0; --ii) {
    const size_t i = ii - 1;
    for (size_t jj = m + 1; jj > 0; --jj) {
      const size_t j = jj - 1;
      if (i == n && j == m)
        continue;

      CoreLcsCell best{0, std::numeric_limits<uint64_t>::max()};

      // Keep a matching token pair at the start of both suffixes.
      if (i < n && j < m && a[i] == b[j]) {
        CoreLcsCell cand = suffix[idx(i + 1, j + 1)];
        ++cand.len;
        if (better(cand, best))
          best = cand;
      }

      // Delete A[i]. In suffix coordinates, deleting this token advances to
      // A[i + 1..), so the crossed gap is ownerDepthGap[i + 1].
      if (i < n) {
        CoreLcsCell cand = suffix[idx(i + 1, j)];
        if (addCostChecked(cand.cost, ownerDepthGap[i + 1], cand.cost) &&
            better(cand, best))
          best = cand;
      }

      // Insert B[j] at the current A suffix boundary. This mirrors the forward
      // insertion rule and charges ownerDepthGap[i].
      if (j < m) {
        CoreLcsCell cand = suffix[idx(i, j + 1)];
        if (addCostChecked(cand.cost, ownerDepthGap[i], cand.cost) &&
            better(cand, best))
          best = cand;
      }

      suffix[idx(i, j)] = best;
    }
  }

  return true;
}

/// Return true iff a provenance id is present rather than the sentinel zero.
static bool hasProvenanceId(uint64_t value) {
  return value != LcsAGapProvenance::noId;
}

/// Rank the amount of original-side structural boundary retained at an
/// insertion gap.
///
/// This is not a lexical-neighbor preference. It uses only the provenance facts
/// that the refolder's boundary policy already reasons about: include depth and
/// identity, conditional group/arm identity, and macro root/leaf identity. A
/// larger rank means the gap preserves a more specific owner boundary.
static uint64_t aBoundaryRetentionRank(ArrayRef<LcsAGapProvenance> profiles,
                                       uint64_t gap) {
  if (gap >= profiles.size())
    return 0;

  const LcsAGapProvenance &profile = profiles[static_cast<size_t>(gap)];

  // Depth carries the coarse owner-boundary strength; identity fields then add
  // small tie-breaking increments without introducing token-spelling bias.
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

/// True when two B gaps expose the same line-boundary shape.
static bool sameBLineShape(const LcsBGapProvenance &lhs,
                           const LcsBGapProvenance &rhs) {
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

/// Rank the edited-side line/gap surface around one B-side gap.
///
/// This is intentionally structural rather than lexical: it never looks at the
/// neighboring token spellings. It is used only after A-side owner-boundary
/// specificity, to separate candidates that preserve the same core LCS proof.
static uint64_t bGapSurfaceRank(ArrayRef<LcsBGapProvenance> profiles,
                                uint64_t gap) {
  if (gap >= profiles.size())
    return 0;

  const LcsBGapProvenance &profile = profiles[static_cast<size_t>(gap)];
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

/// Rank a matched B-token pair using the surrounding B-gap surfaces.
static uint64_t bPairSurfaceRank(ArrayRef<LcsBGapProvenance> profiles,
                                 uint64_t bStart, uint64_t bEnd) {
  uint64_t rank = bGapSurfaceRank(profiles, bStart) +
                  bGapSurfaceRank(profiles, bEnd);
  if (bStart < profiles.size() && bEnd < profiles.size() &&
      sameBLineShape(profiles[static_cast<size_t>(bStart)],
                     profiles[static_cast<size_t>(bEnd)])) {
    rank += 8;
  }
  return rank;
}

/// Remove individually admissible anchors that are not mutually order-
/// compatible.
///
/// The core admissibility pass reasons about one candidate anchor at a time: an
/// A/B token pair may occur in some optimal owner-aware LCS path. In highly
/// repetitive regions, two such individually valid anchors can still be
/// mutually exclusive because they cross in B order. Returning both violates
/// the A->B map contract and later hunk construction is undefined.
///
/// This cleanup is deliberately conservative. It does not pick a longest
/// increasing subsequence, because that would select one of several equally
/// valid repeated-token explanations. Instead, an anchor is retained only if
/// it is order-compatible with every other currently retained anchor: all
/// mapped anchors to its left must map to smaller B indices, and all mapped
/// anchors to its right must map to larger B indices. Any anchor involved in a
/// crossing is suppressed, widening the surrounding edit island and preserving
/// fail-closed behavior.
static size_t suppressOrderConflictingAnchors(std::vector<int64_t> &map) {
  const size_t n = map.size();
  if (n == 0)
    return 0;

  // Prefix scan: for each A index, remember the largest retained B index to
  // its left. A valid anchor must be strictly greater than this value.
  std::vector<int64_t> maxLeft(n, -1);
  int64_t leftMax = -1;
  for (size_t i = 0; i < n; ++i) {
    maxLeft[i] = leftMax;
    if (map[i] >= 0)
      leftMax = std::max(leftMax, map[i]);
  }

  // Suffix scan: symmetrically remember the smallest retained B index to the
  // right. A valid anchor must be strictly smaller than this value.
  std::vector<int64_t> minRight(n, std::numeric_limits<int64_t>::max());
  int64_t rightMin = std::numeric_limits<int64_t>::max();
  for (size_t i = n; i-- > 0;) {
    minRight[i] = rightMin;
    if (map[i] >= 0)
      rightMin = std::min(rightMin, map[i]);
  }

  // Drop any anchor that crosses either side. This deliberately widens hunks
  // instead of choosing one arbitrary explanation for repeated tokens.
  size_t suppressed = 0;
  for (size_t i = 0; i < n; ++i) {
    const int64_t bj = map[i];
    if (bj < 0)
      continue;
    if (maxLeft[i] >= bj || minRight[i] <= bj) {
      map[i] = -1;
      ++suppressed;
    }
  }
  return suppressed;
}

/// Build the heuristic-free provenance-certified LCS map used by refolding.
///
/// The algorithm separates optimality from certification:
///  * compute the core owner-aware LCS objective with no neighbor-spelling tie:
///    maximize length, then minimize ownerDepthGap cost;
///  * retain only anchors forced by that core objective, meaning both sides
///    have exactly one admissible partner;
///  * restore ambiguous equal-token edge anchors only when they create a unique
///    best pure-insertion frontier under structural boundary ranks.
///
/// If a hunk still has no unique boundary-preserving pure-insertion candidate,
/// the ambiguous anchors remain suppressed. That is fail-closed: downstream
/// code sees a wider edit island instead of an arbitrary repeated punctuation
/// anchor.
static bool buildBoundaryPureCertifiedMap(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<uint32_t> ownerDepthGap, ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<LcsBGapProvenance> bGapProvenance,
    unsigned long long maxCells, std::vector<int64_t> &outMap) {
  const size_t n = a.size();
  const size_t m = b.size();
  outMap.assign(n, -1);
  if (n == 0 || m == 0)
    return true;
  if (ownerDepthGap.size() != n + 1 || gapProvenance.size() != n + 1)
    return false;

  // The forward/suffix tables let us ask whether any proposed A/B equal-token
  // pair participates in at least one globally optimal core LCS solution.
  std::vector<CoreLcsCell> forward;
  std::vector<CoreLcsCell> suffix;
  size_t stride = 0;
  if (!buildCoreLcsDpTables(a, b, ownerDepthGap, maxCells, forward, suffix,
                            stride))
    return false;

  auto idx = [&](size_t i, size_t j) -> size_t { return i * stride + j; };
  const CoreLcsCell total = forward[idx(n, m)];

  auto isCoreAdmissible = [&](size_t ai, size_t bj) -> bool {
    // Splice prefix + this match + suffix. The candidate is admissible only if
    // it preserves both the optimal LCS length and the optimal owner-depth cost.
    if (ai >= n || bj >= m || a[ai] != b[bj])
      return false;
    const CoreLcsCell &prefix = forward[idx(ai, bj)];
    const CoreLcsCell &tail = suffix[idx(ai + 1, bj + 1)];
    if (prefix.len + 1U + tail.len != total.len)
      return false;
    if (prefix.cost > std::numeric_limits<uint64_t>::max() - tail.cost)
      return false;
    return prefix.cost + tail.cost == total.cost;
  };

  // Count every individually admissible partner in the optimal core objective.
  // An anchor is forced only when both directions are unique: A[i] can match
  // one B token, and that B token can match only this A token.
  std::vector<uint32_t> aPartnerCount(n, 0);
  std::vector<uint32_t> bPartnerCount(m, 0);
  std::vector<int64_t> firstBPartner(n, -1);
  for (size_t ai = 0; ai < n; ++ai) {
    for (size_t bj = 0; bj < m; ++bj) {
      if (!isCoreAdmissible(ai, bj))
        continue;
      if (aPartnerCount[ai] == 0)
        firstBPartner[ai] = static_cast<int64_t>(bj);
      ++aPartnerCount[ai];
      ++bPartnerCount[bj];
    }
  }

  // Seed the map with only forced anchors. Repeated punctuation/literals that
  // have several optimal explanations remain unmatched until a later boundary
  // proof restores them at an edit edge.
  for (size_t ai = 0; ai < n; ++ai) {
    if (aPartnerCount[ai] != 1)
      continue;
    const int64_t bj = firstBPartner[ai];
    if (bj >= 0 && bj < static_cast<int64_t>(m) &&
        bPartnerCount[static_cast<size_t>(bj)] == 1)
      outMap[ai] = bj;
  }

  // Defensive monotonicity cleanup: individual admissibility is local to a
  // token pair, so suppress any rare crossing anchors before hunk construction.
  suppressOrderConflictingAnchors(outMap);

  // Return true when a matching A/B token pair can serve as an ambiguous edge
  // anchor: it must be core-admissible, but not already a unique one-to-one
  // match that would be handled by the ordinary anchor path.
  auto canUseAmbiguousEdgeAnchor = [&](uint64_t ai64, uint64_t bj64) -> bool {
    if (ai64 >= n || bj64 >= m)
      return false;
    const size_t ai = static_cast<size_t>(ai64);
    const size_t bj = static_cast<size_t>(bj64);
    if (a[ai] != b[bj])
      return false;
    if (!isCoreAdmissible(ai, bj))
      return false;
    return !(aPartnerCount[ai] == 1 && bPartnerCount[bj] == 1);
  };

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

  // Order boundary-pure candidates by the deterministic preference used for
  // anchor selection: best balance first, then stronger boundary evidence, then
  // stronger B-side pair evidence.
  auto betterCandidate = [](const BoundaryPureCandidate &cand,
                            const BoundaryPureCandidate &best) {
    if (cand.balance != best.balance)
      return cand.balance < best.balance;
    if (cand.boundaryRank != best.boundaryRank)
      return cand.boundaryRank > best.boundaryRank;
    return cand.bPairRank > best.bPairRank;
  };

  // Order suffix-insertion candidates by deterministic recovery strength:
  // prefer stronger boundary evidence, stronger B-side pair evidence, more
  // restored anchors, then the rightmost compatible insertion window.
  auto betterSuffixInsertionCandidate =
      [](const SuffixInsertionCandidate &cand,
         const SuffixInsertionCandidate &best) {
        if (cand.boundaryRank != best.boundaryRank)
          return cand.boundaryRank > best.boundaryRank;
        if (cand.bPairRank != best.bPairRank)
          return cand.bPairRank > best.bPairRank;
        if (cand.restoredAnchors != best.restoredAnchors)
          return cand.restoredAnchors > best.restoredAnchors;
        if (cand.right != best.right)
          return cand.right > best.right;
        return cand.left > best.left;
      };

  // Each forced hunk is a conservative edit island. Try to shrink only its
  // edges by restoring ambiguous equal-token anchors when the resulting
  // frontier is uniquely certified as a boundary-preserving pure insertion.
  const std::vector<Hunk> forcedHunks = hunksFromMap(outMap, n, m);
  for (const Hunk &forced : forcedHunks) {
    const uint64_t maxSharedWidth =
        std::min(hunkAWidth(forced), hunkBWidth(forced));
    if (maxSharedWidth == 0)
      continue;

    // Compute the maximal ambiguous equal-token edge runs. These are candidates
    // for restoration; interior ambiguous anchors are intentionally ignored.
    uint64_t maxPrefix = 0;
    while (maxPrefix < maxSharedWidth &&
           canUseAmbiguousEdgeAnchor(forced.aStart + maxPrefix,
                                     forced.bStart + maxPrefix))
      ++maxPrefix;

    uint64_t maxSuffix = 0;
    while (maxSuffix < maxSharedWidth &&
           canUseAmbiguousEdgeAnchor(forced.aEnd - maxSuffix - 1,
                                     forced.bEnd - maxSuffix - 1))
      ++maxSuffix;

    // First handle the symmetric case: restore some left and/or right edge
    // anchors so the remaining hunk is exactly A-empty/B-nonempty.
    bool haveBest = false;
    BoundaryPureCandidate best;
    size_t bestCount = 0;
    for (uint64_t left = 0; left <= maxPrefix; ++left) {
      for (uint64_t right = 0; right <= maxSuffix; ++right) {
        if (left + right > maxSharedWidth)
          continue;
        const uint64_t aStart = forced.aStart + left;
        const uint64_t aEnd = forced.aEnd - right;
        const uint64_t bStart = forced.bStart + left;
        const uint64_t bEnd = forced.bEnd - right;
        if (aStart > aEnd || bStart > bEnd)
          continue;
        if (!(aStart == aEnd && bStart < bEnd))
          continue;

        BoundaryPureCandidate cand;
        cand.left = left;
        cand.right = right;
        cand.balance = left > right ? left - right : right - left;
        cand.boundaryRank = aBoundaryRetentionRank(gapProvenance, aStart);
        cand.bPairRank = bPairSurfaceRank(bGapProvenance, bStart, bEnd);
        if (!haveBest || betterCandidate(cand, best)) {
          best = cand;
          haveBest = true;
          bestCount = 1;
        } else if (cand.balance == best.balance &&
                   cand.boundaryRank == best.boundaryRank &&
                   cand.bPairRank == best.bPairRank) {
          ++bestCount;
        }
      }
    }

    if (!haveBest) {
      // A boundary-pure insertion can also be hidden behind an unchanged token
      // immediately to the left of the insertion frontier. This occurs when a
      // line-local macro expansion is edited and a B-only after-boundary
      // payload is appended before the next unchanged token. Equal-offset edge
      // restoration cannot prove that shape because the delimiter token is no
      // longer at the same relative B offset, but the shape is still fully
      // certifiable:
      //
      //   [left edit]  anchor  [B-only suffix insertion]  [right edge anchors]
      //
      // The restored internal anchor must be core-LCS-admissible, the suffix
      // insertion must be bounded by at least one restored right-edge anchor,
      // and the selected frontier must be unique under structural A-gap and
      // B-gap ranks. No neighboring token spelling is consulted to choose the
      // frontier; spellings are used only for ordinary LCS anchor equality.
      bool haveSuffixBest = false;
      SuffixInsertionCandidate suffixBest;
      size_t suffixBestCount = 0;

      // Enumerate possible right-edge restorations first. `right` gives the
      // number of unchanged trailing anchors that bound the B-only suffix
      // insertion from the right.
      for (uint64_t right = 1; right <= maxSuffix; ++right) {
        const uint64_t aPureGap = forced.aEnd - right;
        const uint64_t bPureEnd = forced.bEnd - right;
        if (aPureGap == 0 || aPureGap <= forced.aStart ||
            bPureEnd <= forced.bStart)
          continue;

        // The insertion frontier is immediately after this restored internal
        // anchor. The anchor is allowed to move on the B side, unlike the
        // equal-offset edge anchors handled by the earlier path.
        const uint64_t anchorA = aPureGap - 1;
        for (uint64_t left = 0; left <= maxPrefix; ++left) {
          if (left + right + 1 > maxSharedWidth)
            continue;
          const uint64_t leftA = forced.aStart + left;
          const uint64_t leftB = forced.bStart + left;
          if (anchorA < leftA)
            continue;

          for (uint64_t anchorB = leftB; anchorB < bPureEnd; ++anchorB) {
            if (!canUseAmbiguousEdgeAnchor(anchorA, anchorB))
              continue;

            const uint64_t insertionBegin = anchorB + 1;
            if (insertionBegin >= bPureEnd)
              continue;

            // Rank the candidate by the structural boundary it preserves and
            // by the B-only surface that would be inserted. The comparator
            // below uses only these proof ranks plus deterministic tie-breaks.
            SuffixInsertionCandidate cand;
            cand.left = left;
            cand.right = right;
            cand.anchorA = anchorA;
            cand.anchorB = anchorB;
            cand.boundaryRank = aBoundaryRetentionRank(gapProvenance, aPureGap);
            cand.bPairRank =
                bPairSurfaceRank(bGapProvenance, insertionBegin, bPureEnd);
            cand.restoredAnchors = left + right + 1;

            if (!haveSuffixBest ||
                betterSuffixInsertionCandidate(cand, suffixBest)) {
              suffixBest = cand;
              haveSuffixBest = true;
              suffixBestCount = 1;
            } else if (cand.boundaryRank == suffixBest.boundaryRank &&
                       cand.bPairRank == suffixBest.bPairRank &&
                       cand.restoredAnchors == suffixBest.restoredAnchors &&
                       cand.right == suffixBest.right &&
                       cand.left == suffixBest.left) {
              ++suffixBestCount;
            }
          }
        }
      }

      if (!haveSuffixBest)
        continue;
      if (suffixBestCount != 1) {
        continue;
      }

      // Commit only the anchors proven by the suffix-insertion certificate:
      // unchanged prefix anchors, the moved internal anchor, and unchanged
      // suffix anchors. The B-only interval between anchorB and the right edge
      // intentionally remains unmapped.
      for (uint64_t off = 0; off < suffixBest.left; ++off) {
        outMap[static_cast<size_t>(forced.aStart + off)] =
            static_cast<int64_t>(forced.bStart + off);
      }
      outMap[static_cast<size_t>(suffixBest.anchorA)] =
          static_cast<int64_t>(suffixBest.anchorB);
      for (uint64_t off = 0; off < suffixBest.right; ++off) {
        outMap[static_cast<size_t>(forced.aEnd - suffixBest.right + off)] =
            static_cast<int64_t>(forced.bEnd - suffixBest.right + off);
      }

      continue;
    }

    if (bestCount != 1) {
      continue;
    }

    // Restore only the uniquely certified equal-token edge runs. The middle of
    // the hunk remains B-only, which downstream code can treat as an insertion.
    for (uint64_t off = 0; off < best.left; ++off) {
      outMap[static_cast<size_t>(forced.aStart + off)] =
          static_cast<int64_t>(forced.bStart + off);
    }
    for (uint64_t off = 0; off < best.right; ++off) {
      outMap[static_cast<size_t>(forced.aEnd - best.right + off)] =
          static_cast<int64_t>(forced.bEnd - best.right + off);
    }
  }

  // The final map must remain a strict A→B monotone alignment. If an edge
  // restoration unexpectedly crosses another anchor, discard restored ambiguous
  // edges and return to the fail-closed forced-anchor map.
  int64_t previousB = -1;
  for (size_t ai = 0; ai < outMap.size(); ++ai) {
    const int64_t bj = outMap[ai];
    if (bj < 0)
      continue;
    if (bj <= previousB) {

      // Rebuild the fallback map from only unique one-to-one token partners.
      // This intentionally drops every ambiguity-restored edge and keeps only
      // anchors that are independently forced by both A-side and B-side
      // uniqueness.
      outMap.assign(n, -1);
      for (size_t forcedAi = 0; forcedAi < n; ++forcedAi) {
        if (aPartnerCount[forcedAi] != 1)
          continue;
        const int64_t forcedBj = firstBPartner[forcedAi];
        if (forcedBj >= 0 && forcedBj < static_cast<int64_t>(m) &&
            bPartnerCount[static_cast<size_t>(forcedBj)] == 1)
          outMap[forcedAi] = forcedBj;
      }

      // Even the unique-partner fallback must be order-clean before it leaves
      // this function.
      suppressOrderConflictingAnchors(outMap);
      return true;
    }
    previousB = bj;
  }

  return true;
}

// ===================== Hirschberg (exact, linear space) ======================

struct Score {
  uint32_t len = 0;
  uint64_t cost = 0;

  Score operator+(const Score &other) const {
    Score out;
    out.len = len + other.len;
    out.cost = cost + other.cost;
    return out;
  }

  bool operator==(const Score &other) const {
    return len == other.len && cost == other.cost;
  }
};

struct SpanView {
  ArrayRef<StringRef> base;
  size_t off = 0;
  size_t len = 0;
  bool rev = false;

  size_t size() const { return len; }

  StringRef at(size_t i) const {
    return rev ? base[off + (len - 1 - i)] : base[off + i];
  }

  size_t absIndex(size_t i) const {
    return rev ? (off + (len - 1 - i)) : (off + i);
  }
};

struct GapView {
  ArrayRef<uint32_t> base;
  size_t off = 0;
  size_t len = 0; // boundaries, so len == A.len + 1 for the corresponding span
  bool rev = false;

  size_t size() const { return len; }

  uint32_t at(size_t i) const {
    return rev ? base[off + (len - 1 - i)] : base[off + i];
  }
};

/// Compute one weighted Hirschberg LCS DP row for the given span views.
static std::vector<Score>
computeRowWeighted(const SpanView &aV, const SpanView &bV,
                   const GapView &gapV) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (gapV.size() != n + 1)
    REFOLD_LOG_FATAL("lcs/map", "internal: gap view length must be A.len+1");

  std::vector<Score> dp(m + 1);
  std::vector<Score> ndp(m + 1);

  dp[0] = Score{0, 0};
  // Row 0: only insertions, charged at boundary 0.
  for (size_t j = 1; j <= m; ++j) {
    dp[j] = dp[j - 1];
    dp[j].cost += static_cast<uint64_t>(gapV.at(0));
  }

  for (size_t i = 1; i <= n; ++i) {
    // Col 0: only deletions, charged at boundary i.
    ndp[0] = dp[0];
    ndp[0].cost += static_cast<uint64_t>(gapV.at(i));

    Score diagPrev = dp[0];
    for (size_t j = 1; j <= m; ++j) {
      Score best;
      best.len = 0;
      best.cost = std::numeric_limits<uint64_t>::max();
      // 1) Match (diag)
      if (aV.at(i - 1) == bV.at(j - 1)) {
        Score cand = diagPrev;
        cand.len += 1U;
        best = cand;
      }

      // 2) Delete A (from dp[j])
      {
        Score cand = dp[j];
        cand.cost += static_cast<uint64_t>(gapV.at(i));
        if (isCoreBetter(cand.len, cand.cost, best.len, best.cost)) {
          best = cand;
        }
      }

      // 3) Insert B (from ndp[j-1])
      {
        Score cand = ndp[j - 1];
        cand.cost += static_cast<uint64_t>(gapV.at(i));
        if (isCoreBetter(cand.len, cand.cost, best.len, best.cost)) {
          best = cand;
        }
      }

      diagPrev = dp[j];
      ndp[j] = best;
    }

    dp.swap(ndp);
  }

  return dp;
}

/// Solve a small weighted-LCS box with the full DP table and append anchors in
/// forward order.
static void solveSmallWeightedDP(const SpanView &aV, const SpanView &bV,
                                 const GapView &gapV,
                                 std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (gapV.size() != n + 1)
    REFOLD_LOG_FATAL("lcs/map", "internal: gap view length must be A.len+1");

  const size_t stride = m + 1;
  const size_t cells = (n + 1) * (m + 1);

  std::vector<uint32_t> dpLen(cells, 0);
  std::vector<uint64_t> dpCost(cells, 0);
  auto idx = [&](size_t i, size_t j) -> size_t { return i * stride + j; };
  auto len = [&](size_t i, size_t j) -> uint32_t & { return dpLen[idx(i, j)]; };
  auto cost = [&](size_t i, size_t j) -> uint64_t & {
    return dpCost[idx(i, j)];
  };
  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      if (i == 0 && j == 0)
        continue;

      uint32_t bestLen = 0;
      uint64_t bestCost = std::numeric_limits<uint64_t>::max();
      // Match
      if (i > 0 && j > 0 && aV.at(i - 1) == bV.at(j - 1)) {
        bestLen = len(i - 1, j - 1) + 1U;
        bestCost = cost(i - 1, j - 1);
      }

      // Delete A (pay at boundary i)
      if (i > 0) {
        const uint32_t candLen = len(i - 1, j);
        const uint64_t candCost =
            cost(i - 1, j) + static_cast<uint64_t>(gapV.at(i));
        if (isCoreBetter(candLen, candCost, bestLen, bestCost)) {
          bestLen = candLen;
          bestCost = candCost;
        }
      }

      // Insert B (pay at boundary i)
      if (j > 0) {
        const uint32_t candLen = len(i, j - 1);
        const uint64_t candCost =
            cost(i, j - 1) + static_cast<uint64_t>(gapV.at(i));
        if (isCoreBetter(candLen, candCost, bestLen, bestCost)) {
          bestLen = candLen;
          bestCost = candCost;
        }
      }

      len(i, j) = bestLen;
      cost(i, j) = bestCost;
    }
  }

  // Backtrack (diag, then up, then left)
  size_t i = n;
  size_t j = m;
  while (i > 0 || j > 0) {
    const uint32_t curLen = len(i, j);
    const uint64_t curCost = cost(i, j);
    bool moved = false;

    // Diagonal match
    if (i > 0 && j > 0 && aV.at(i - 1) == bV.at(j - 1)) {
      if (len(i - 1, j - 1) == curLen - 1U &&
          cost(i - 1, j - 1) == curCost) {
        outMap[aV.absIndex(i - 1)] = static_cast<int64_t>(bV.absIndex(j - 1));
        --i;
        --j;
        moved = true;
      }
    }

    // Up (delete A)
    if (!moved && i > 0) {
      if (len(i - 1, j) == curLen &&
          cost(i - 1, j) + static_cast<uint64_t>(gapV.at(i)) == curCost) {
        --i;
        moved = true;
      }
    }

    // Left (insert B)
    if (!moved && j > 0) {
      if (len(i, j - 1) == curLen &&
          cost(i, j - 1) + static_cast<uint64_t>(gapV.at(i)) == curCost) {
        --j;
        moved = true;
      }
    }

    if (!moved)
      break;
  }
}

/// Recursively solve weighted LCS with Hirschberg splitting.
///
/// This is the structural-cost counterpart to the unweighted Hirschberg solver.
/// It avoids materializing a full quadratic DP table for large boxes by
/// splitting A in half, choosing the B split that maximizes the weighted core
/// objective, and recursively solving the two induced subproblems. The
/// objective is the same one used by the exact weighted DP path: maximize LCS
/// length, then minimize owner-depth gap cost.
static void hirschbergWeightedRec(const SpanView &aV, const SpanView &bV,
                                  const GapView &gapV,
                                  std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (n == 0 || m == 0)
    return;

  // Small exact DP base case.
  const unsigned long long cells = static_cast<unsigned long long>(n + 1ULL) *
                                   static_cast<unsigned long long>(m + 1ULL);
  if (cells <= (1ULL << 20)) {
    solveSmallWeightedDP(aV, bV, gapV, outMap);
    return;
  }

  // Split A in half. The gap view is split with one extra element on each side
  // because A-side gap costs are indexed at token boundaries, so an A span of
  // length k owns k + 1 gap positions.
  const size_t mid = n / 2;

  const SpanView aLeft{aV.base, aV.off, mid, aV.rev};
  const GapView gapLeft{gapV.base, gapV.off, mid + 1, gapV.rev};

  const SpanView aRight{aV.base, aV.off + mid, n - mid, aV.rev};
  const GapView gapRight{gapV.base, gapV.off + mid, (n - mid) + 1, gapV.rev};

  // Compute the best weighted LCS objective for every possible B split after
  // solving the left half of A against each prefix of B.
  const std::vector<Score> leftRow =
      computeRowWeighted(aLeft, bV, gapLeft);

  // Compute the corresponding suffix objectives by solving the right half of A
  // and B in reverse. rightRowRev[m - j] is the score for A[mid..n) against
  // B[j..m).
  const SpanView aRightRev{aV.base, aV.off + mid, n - mid, true};
  const GapView gapRightRev{gapV.base, gapV.off + mid, (n - mid) + 1, true};
  const SpanView bRev{bV.base, bV.off, m, true};

  const std::vector<Score> rightRowRev =
      computeRowWeighted(aRightRev, bRev, gapRightRev);

  // Choose split j maximizing the core objective (length, then inverse cost).
  // On exact equality, prefer the smallest j for determinism.
  size_t bestJ = 0;
  Score best = leftRow[0] + rightRowRev[m];
  for (size_t j = 1; j <= m; ++j) {
    Score cand = leftRow[j] + rightRowRev[m - j];
    if (isCoreBetter(cand.len, cand.cost, best.len, best.cost) ||
        (cand == best && j < bestJ)) {
      bestJ = j;
      best = cand;
    }
  }

  // Recurse on the two independently optimal subproblems induced by the chosen
  // B split. Both calls write their anchors into the shared global output map.
  const SpanView bLeft{bV.base, bV.off, bestJ, bV.rev};
  const SpanView bRight{bV.base, bV.off + bestJ, m - bestJ, bV.rev};

  hirschbergWeightedRec(aLeft, bLeft, gapLeft, outMap);
  hirschbergWeightedRec(aRight, bRight, gapRight, outMap);
}

/// Entry point for the linear-space weighted LCS fallback.
static std::vector<int64_t>
lcsMapABHirschbergWeighted(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                           ArrayRef<uint32_t> ownerDepthGap) {
  std::vector<int64_t> out(a.size(), -1);
  const SpanView aV{a, 0, a.size(), false};
  const SpanView bV{b, 0, b.size(), false};
  const GapView gV{ownerDepthGap, 0, ownerDepthGap.size(), false};
  hirschbergWeightedRec(aV, bV, gV, out);
  return out;
}

/// Compute one unweighted Hirschberg LCS length row.
///
/// This is the length-only row builder used by the non-policy overload. It
/// computes the final DP row for `aV` against every prefix of `bV`, using only
/// two rows of storage so Hirschberg can choose a split without materializing
/// the full table.
static std::vector<unsigned> computeRowLen(const SpanView &aV,
                                           const SpanView &bV) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  std::vector<unsigned> dp(m + 1, 0);
  std::vector<unsigned> ndp(m + 1, 0);

  // `dp` is the previous A-row and `ndp` is the row being built. `diagPrev`
  // carries the old dp[j - 1] value needed for the match transition.
  for (size_t i = 1; i <= n; ++i) {
    ndp[0] = 0;
    unsigned diagPrev = 0;
    for (size_t j = 1; j <= m; ++j) {
      const unsigned up = dp[j];
      const unsigned left = ndp[j - 1];
      const unsigned diag = diagPrev;
      diagPrev = dp[j];
      if (aV.at(i - 1) == bV.at(j - 1))
        ndp[j] = diag + 1U;
      else
        ndp[j] = (up >= left) ? up : left;
    }
    dp.swap(ndp);
  }
  return dp;
}

/// Solve a small unweighted LCS subproblem with a full suffix-DP table.
///
/// This is the exact base case for the non-policy Hirschberg path. It
/// materializes the complete DP table for the current `SpanView` box, then
/// backtracks greedily through that table to emit A→B anchors into the shared
/// absolute `outMap`.
static void solveSmallUnweightedDP(const SpanView &aV, const SpanView &bV,
                                   std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  const size_t stride = m + 1;
  const size_t cells = (n + 1) * (m + 1);
  std::vector<unsigned> dp(cells, 0);
  auto dpLocal = [&](size_t i, size_t j) -> unsigned & {
    return dp[i * stride + j];
  };

  // Build suffix DP: DP(i, j) is the LCS length of A[i..n) and B[j..m).
  for (size_t i = n; i-- > 0;) {
    for (size_t j = m; j-- > 0;) {
      dpLocal(i, j) = (aV.at(i) == bV.at(j)) ? dpLocal(i + 1, j + 1) + 1U
                                        : std::max(dpLocal(i + 1, j), dpLocal(i, j + 1));
    }
  }

  // Walk one deterministic optimal path through the table and record matches
  // using absolute indices from the original token streams.
  size_t i = 0, j = 0;
  while (i < n && j < m) {
    if (aV.at(i) == bV.at(j)) {
      outMap[aV.absIndex(i)] = static_cast<int64_t>(bV.absIndex(j));
      ++i;
      ++j;
    } else if (dpLocal(i + 1, j) >= dpLocal(i, j + 1)) {
      ++i;
    } else {
      ++j;
    }
  }
}

/// Recursively solve unweighted LCS with Hirschberg splitting.
///
/// This is the length-only counterpart to the weighted Hirschberg solver. It
/// avoids materializing a full quadratic DP table for large boxes by splitting
/// A in half, choosing the B split that preserves the maximum LCS length, and
/// recursively solving the two induced subproblems.
static void hirschbergUnweightedRec(const SpanView &aV, const SpanView &bV,
                                    std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (n == 0 || m == 0)
    return;

  // Small exact DP base case.
  const unsigned long long cells = static_cast<unsigned long long>(n + 1ULL) *
                                   static_cast<unsigned long long>(m + 1ULL);
  if (cells <= (1ULL << 20)) {
    solveSmallUnweightedDP(aV, bV, outMap);
    return;
  }

  // Split A in half. Hirschberg only materializes one DP row per side, so this
  // keeps memory linear in |B| while preserving the exact LCS objective.
  const size_t mid = n / 2;
  const SpanView aLeft{aV.base, aV.off, mid, aV.rev};
  const SpanView aRight{aV.base, aV.off + mid, n - mid, aV.rev};

  // Compute the best unweighted LCS length for the left half of A against every
  // prefix of B.
  const std::vector<unsigned> leftRow = computeRowLen(aLeft, bV);

  // Compute suffix objectives by solving the right half of A and B in reverse.
  // rightRowRev[m - j] is the LCS length of A[mid..n) against B[j..m).
  const SpanView aRightRev{aV.base, aV.off + mid, n - mid, true};
  const SpanView bRev{bV.base, bV.off, m, true};
  const std::vector<unsigned> rightRowRev = computeRowLen(aRightRev, bRev);

  // Choose the B split that maximizes the total LCS length. On ties, prefer the
  // smallest split index for deterministic output.
  size_t bestJ = 0;
  unsigned bestLen = leftRow[0] + rightRowRev[m];
  for (size_t j = 1; j <= m; ++j) {
    const unsigned candLen = leftRow[j] + rightRowRev[m - j];
    if (candLen > bestLen || (candLen == bestLen && j < bestJ)) {
      bestLen = candLen;
      bestJ = j;
    }
  }

  // Recurse on the two subproblems induced by the chosen B split. Both calls
  // write their anchors into the shared global output map.
  const SpanView bLeft{bV.base, bV.off, bestJ, bV.rev};
  const SpanView bRight{bV.base, bV.off + bestJ, m - bestJ, bV.rev};

  hirschbergUnweightedRec(aLeft, bLeft, outMap);
  hirschbergUnweightedRec(aRight, bRight, outMap);
}

/// Entry point for the linear-space unweighted LCS fallback.
static std::vector<int64_t> lcsMapABHirschberg(ArrayRef<StringRef> a,
                                               ArrayRef<StringRef> b) {
  std::vector<int64_t> out(a.size(), -1);
  const SpanView aV{a, 0, a.size(), false};
  const SpanView bV{b, 0, b.size(), false};
  hirschbergUnweightedRec(aV, bV, out);
  return out;
}
} // namespace

// ================ Weighted LCS (DP with Hirschberg fallback) =================

std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<uint32_t> ownerDepthGap,
                              unsigned long long maxCells) {
  using namespace clang::refold;

  const size_t n = a.size(), m = b.size();

  // Early outs for empties.
  if (n == 0)
    return {};
  if (m == 0)
    return std::vector<int64_t>(n, -1);

  // Validate ownerDepthGap shape.
  if (ownerDepthGap.size() != n + 1) {
    REFOLD_LOG_FATAL("lcs/map", "ownerDepthGap length must be A.size() + 1");
  }

  // Indices are stored in int64_t; extremely large B streams are not
  // representable.
  if (m > MAX)
    REFOLD_LOG_FATAL("lcs/map", "B.size() exceeds int64_t index range");

  // DP table guard: if the full (n+1)*(m+1) table is too large, use Hirschberg
  // to remain exact while using only O(n+m) memory.
  const unsigned long long nu = static_cast<unsigned long long>(n);
  const unsigned long long mu = static_cast<unsigned long long>(m);
  const bool useHirschberg = shouldUseGreedyApproach(nu, mu, maxCells);
  if (useHirschberg) {
    std::vector<int64_t> map = lcsMapABHirschbergWeighted(a, b, ownerDepthGap);
    return map;
  }

  // ---------------- DP path: (n+1) x (m+1) tables, row-major -----------------
  // dpLen(i,j)  = max LCS length for A[0..i) vs B[0..j)
  // dpCost(i,j) = min accumulated gap cost among paths achieving dpLen(i,j)
  //
  // Transitions into (i,j):
  //   match:     from (i-1,j-1), +1 length, +0 cost
  //   delete A:  from (i-1,j),   +0 length, +ownerDepthGap[i]
  //   insert B:  from (i,  j-1), +0 length, +ownerDepthGap[i]
  const size_t stride = m + 1;
  const size_t cells = (n + 1) * (m + 1);

  std::vector<uint32_t> dpLen(cells, 0);
  // Using uint64_t for cost prevents overflow during accumulation.
  std::vector<uint64_t> dpCost(cells, 0);

  auto idx = [&](size_t i, size_t j) -> size_t { return i * stride + j; };
  auto len = [&](size_t i, size_t j) -> uint32_t & { return dpLen[idx(i, j)]; };
  auto cost = [&](size_t i, size_t j) -> uint64_t & {
    return dpCost[idx(i, j)];
  };

  // Fill DP table forward. Equal-core predecessor states keep the first
  // considered transition, preserving the existing deterministic order:
  // diagonal, then delete, then insert.
  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      if (i == 0 && j == 0)
        continue;

      uint32_t bestLen = 0;
      uint64_t bestCost = std::numeric_limits<uint64_t>::max();

      // 1) Match (diagonal).
      if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
        bestLen = len(i - 1, j - 1) + 1U;
        bestCost = cost(i - 1, j - 1);
      }

      // 2) Delete A (vertical move: i-1 -> i), pay ownerDepthGap[i].
      if (i > 0) {
        const uint32_t candLen = len(i - 1, j);
        const uint64_t candCost =
            cost(i - 1, j) + static_cast<uint64_t>(ownerDepthGap[i]);
        if (isCoreBetter(candLen, candCost, bestLen, bestCost)) {
          bestLen = candLen;
          bestCost = candCost;
        }
      }

      // 3) Insert B (horizontal move: j-1 -> j), pay ownerDepthGap[i].
      if (j > 0) {
        const uint32_t candLen = len(i, j - 1);
        const uint64_t candCost =
            cost(i, j - 1) + static_cast<uint64_t>(ownerDepthGap[i]);
        if (isCoreBetter(candLen, candCost, bestLen, bestCost)) {
          bestLen = candLen;
          bestCost = candCost;
        }
      }

      len(i, j) = bestLen;
      cost(i, j) = bestCost;
    }
  }

  // ------------------- Backtrack: diag, then up, then left -------------------
  std::vector<int64_t> map(n, -1);

  size_t i = n;
  size_t j = m;
  while (i > 0 || j > 0) {
    const uint32_t curLen = len(i, j);
    const uint64_t curCost = cost(i, j);

    bool moved = false;

    // Diagonal (match).
    if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
      if (len(i - 1, j - 1) == curLen - 1U &&
          cost(i - 1, j - 1) == curCost) {
        map[i - 1] = static_cast<int64_t>(j - 1);
        --i;
        --j;
        moved = true;
      }
    }

    // Up (delete A): from (i-1, j) paying ownerDepthGap[i].
    if (!moved && i > 0) {
      if (len(i - 1, j) == curLen &&
          cost(i - 1, j) + static_cast<uint64_t>(ownerDepthGap[i]) == curCost) {
        --i;
        moved = true;
      }
    }

    // Left (insert B): from (i, j-1) paying ownerDepthGap[i].
    if (!moved && j > 0) {
      if (len(i, j - 1) == curLen &&
          cost(i, j - 1) + static_cast<uint64_t>(ownerDepthGap[i]) == curCost) {
        --j;
        moved = true;
      }
    }

    if (!moved)
      break;
  }

  return map;
}

std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<LcsAGapProvenance> gapProvenance,
                              unsigned long long maxCells) {
  if (gapProvenance.size() != a.size() + 1)
    REFOLD_LOG_FATAL("lcs/map", "gapProvenance length must be A.size() + 1");

  std::vector<uint32_t> ownerDepthGap;
  ownerDepthGap.reserve(gapProvenance.size());
  for (const LcsAGapProvenance &profile : gapProvenance)
    ownerDepthGap.push_back(profile.ownerDepth);

  // A caller without edited-side gap profiles still receives the certified
  // ambiguity-suppressed map. The B-side ranks collapse to zero, so ambiguous
  // edge anchors are restored only when A-side provenance alone proves a unique
  // frontier.
  std::vector<LcsBGapProvenance> emptyBGapProvenance(b.size() + 1);
  std::vector<int64_t> map;
  if (buildBoundaryPureCertifiedMap(a, b, ownerDepthGap, gapProvenance,
                                    emptyBGapProvenance, maxCells, map)) {
    return map;
  }

  // If the full admissibility tables exceed the configured DP budget, fall back
  // to the exact weighted Hirschberg/core-DP implementation. This path uses
  // the same core objective without the removed neighbor-coherence heuristic;
  // it merely lacks the full ambiguity-certification table.
  return lcsMapAB(a, b, ArrayRef<uint32_t>(ownerDepthGap), maxCells);
}

std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<LcsAGapProvenance> gapProvenance,
                              ArrayRef<LcsBGapProvenance> bGapProvenance,
                              unsigned long long maxCells) {
  if (bGapProvenance.size() != b.size() + 1)
    REFOLD_LOG_FATAL("lcs/map", "bGapProvenance length must be B.size() + 1");

  if (gapProvenance.size() != a.size() + 1)
    REFOLD_LOG_FATAL("lcs/map", "gapProvenance length must be A.size() + 1");

  std::vector<uint32_t> ownerDepthGap;
  ownerDepthGap.reserve(gapProvenance.size());
  for (const LcsAGapProvenance &profile : gapProvenance)
    ownerDepthGap.push_back(profile.ownerDepth);

  std::vector<int64_t> map;
  if (buildBoundaryPureCertifiedMap(a, b, ownerDepthGap, gapProvenance,
                                    bGapProvenance, maxCells, map)) {
    return map;
  }

  // See the A-only overload above: this is the exact core fallback, not a
  // return to the old neighbor-coherence heuristic.
  return lcsMapAB(a, b, ArrayRef<uint32_t>(ownerDepthGap), maxCells);
}

[[maybe_unused]]
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              unsigned long long maxCells) {
  const size_t n = a.size(), m = b.size();

  // Early outs for empties
  if (n == 0)
    return {};
  if (m == 0)
    return std::vector<int64_t>(n, -1);

  // Indices are stored in int64_t; extremely large B streams are not
  // representable.
  if (m > MAX)
    REFOLD_LOG_FATAL("lcs/map", "B.size() exceeds int64_t index range");

  // DP table guard: if the full table is too large, use Hirschberg (exact,
  // linear space).
  const unsigned long long nu = static_cast<unsigned long long>(n);
  const unsigned long long mu = static_cast<unsigned long long>(m);
  const bool useHirschberg = shouldUseGreedyApproach(nu, mu, maxCells);
  if (useHirschberg)
    return lcsMapABHirschberg(a, b);

  // ----------------- DP path: (n+1) x (m+1) table, row-major -----------------
  const size_t stride = m + 1;
  const size_t cells = (n + 1) * (m + 1);
  std::vector<unsigned> dp(cells, 0);

  auto dpLocal = [&](size_t i, size_t j) -> unsigned & {
    return dp[i * stride + j];
  };

  // DP(i,j) = LCS length of a[i:] vs b[j:]
  for (size_t i = n; i-- > 0;) {
    for (size_t j = m; j-- > 0;) {
      dpLocal(i, j) = (a[i] == b[j]) ? static_cast<unsigned>(dpLocal(i + 1, j + 1) + 1U)
                                : std::max(dpLocal(i + 1, j), dpLocal(i, j + 1));
    }
  }

  // Reconstruct a->b map with deterministic tie-breaker (>= → skip a)
  std::vector<int64_t> map(n, -1);
  size_t i = 0, j = 0;
  while (i < n && j < m) {
    if (a[i] == b[j]) {
      map[i] = static_cast<int64_t>(j);
      ++i;
      ++j;
    } else if (dpLocal(i + 1, j) >= dpLocal(i, j + 1)) {
      ++i; // skip a[i]
    } else {
      ++j; // skip b[j]
    }
  }
  return map;
}

std::vector<Hunk> hunksFromMap(ArrayRef<int64_t> map, size_t nA, size_t nB) {
  std::vector<Hunk> hunks;

  const uint64_t limitA = static_cast<uint64_t>(nA);
  const uint64_t limitB = static_cast<uint64_t>(nB);

  // Track the 'next expected' index to identify gaps.
  uint64_t nextA = 0;
  uint64_t nextB = 0;
  for (uint64_t i = 0; i < nA; ++i) {
    const int64_t matchedJ = (i < map.size() ? map[i] : -1);
    if (matchedJ < 0)
      continue; // Skip unmatched tokens

    const uint64_t j = static_cast<uint64_t>(matchedJ);

    // If there's a gap in A or B since the last match, we have a Hunk
    if (i > nextA || j > nextB) {
      hunks.push_back(Hunk{nextA, i, nextB, j});
    }

    // After a match, the next possible Hunk starts at i+1, j+1
    nextA = i + 1;
    nextB = j + 1;
  }

  // Handle the tail region
  if (nextA < limitA || nextB < limitB) {
    hunks.push_back(Hunk{nextA, limitA, nextB, limitB});
  }

  return hunks;
}

// ===================== Myers linear-space O((N+M)*D) diff ====================

namespace {
struct MiddleSnake {
  int64_t aStart = 0;
  int64_t bStart = 0;
  int64_t aEnd = 0;
  int64_t bEnd = 0;
};

/// Append one equal run to the SES, if non-empty.
static void appendEqualSteps(std::vector<Step> &out, uint64_t aLo, uint64_t bLo,
                             uint64_t len) {
  for (uint64_t i = 0; i < len; ++i) {
    out.push_back(
        Step{Op::Equal, aLo + i, aLo + i + 1, bLo + i, bLo + i + 1});
  }
}

/// Append one insertion run to the SES, if non-empty.
static void appendInsertSteps(std::vector<Step> &out, uint64_t aPos,
                              uint64_t bLo, uint64_t bHi) {
  for (uint64_t j = bLo; j < bHi; ++j)
    out.push_back(Step{Op::Insert, aPos, aPos, j, j + 1});
}

/// Append one deletion run to the SES, if non-empty.
static void appendDeleteSteps(std::vector<Step> &out, uint64_t aLo,
                              uint64_t aHi, uint64_t bPos) {
  for (uint64_t i = aLo; i < aHi; ++i)
    out.push_back(Step{Op::Delete, i, i + 1, bPos, bPos});
}

/// \brief Find Myers' middle snake for the box A[aLo,aHi) × B[bLo,bHi).
///
/// This is the linear-space divide-and-conquer core of Myers' shortest edit
/// script algorithm. It runs simultaneous forward and reverse frontier sweeps
/// until the two searches overlap, at which point the overlapping diagonal
/// segment ("middle snake") splits the problem into two strictly smaller
/// subproblems.
///
/// The returned coordinates are absolute indices into \p a and \p b and obey:
/// * A[aStart,aEnd) == B[bStart,bEnd)
/// * (aEnd - aStart) == (bEnd - bStart)
/// * The left subproblem is A[aLo,aStart) × B[bLo,bStart)
/// * The right subproblem is A[aEnd,aHi) × B[bEnd,bHi)
/// Find the middle snake used by Myers' divide-and-conquer SES recursion.
static MiddleSnake findMiddleSnake(ArrayRef<StringRef> a, int64_t aLo,
                                   int64_t aHi, ArrayRef<StringRef> b,
                                   int64_t bLo, int64_t bHi) {
  const int64_t n = aHi - aLo;
  const int64_t m = bHi - bLo;
  const int64_t delta = n - m;
  const bool oddDelta = (delta & 1) != 0;
  const int64_t maxD = (n + m + 1) / 2;
  const int64_t offset = maxD + 1;
  const size_t vecSize = static_cast<size_t>(2 * maxD + 3);

  std::vector<int64_t> vf(vecSize, -1);
  std::vector<int64_t> vr(vecSize, -1);

  // Same initialization convention as the classic O(ND) frontier walk: the
  // "virtual" diagonal just above k=0 starts at x=0.
  vf[static_cast<size_t>(offset + 1)] = 0;
  vr[static_cast<size_t>(offset + 1)] = 0;

  for (int64_t d = 0; d <= maxD; ++d) {
    // Forward search from (aLo, bLo).
    for (int64_t k = -d; k <= d; k += 2) {
      const size_t kIdx = static_cast<size_t>(k + offset);

      int64_t xStart;
      if (k == -d || (k != d && vf[kIdx - 1] < vf[kIdx + 1])) {
        xStart = vf[kIdx + 1]; // down: INSERT from B
      } else {
        xStart = vf[kIdx - 1] + 1; // right: DELETE from A
      }

      int64_t yStart = xStart - k;
      int64_t x = xStart;
      int64_t y = yStart;
      while (x < n && y < m &&
             a[static_cast<size_t>(aLo + x)] ==
                 b[static_cast<size_t>(bLo + y)]) {
        ++x;
        ++y;
      }
      vf[kIdx] = x;

      if (oddDelta) {
        const int64_t revK = delta - k;
        if (revK >= -(d - 1) && revK <= (d - 1) &&
            x + vr[static_cast<size_t>(revK + offset)] >= n) {
          return MiddleSnake{aLo + xStart, bLo + yStart, aLo + x, bLo + y};
        }
      }
    }

    // Reverse search from (aHi, bHi), expressed as distances from the end.
    for (int64_t revK = -d; revK <= d; revK += 2) {
      const size_t revIdx = static_cast<size_t>(revK + offset);

      int64_t xEnd;
      if (revK == -d || (revK != d && vr[revIdx - 1] < vr[revIdx + 1])) {
        xEnd = vr[revIdx + 1]; // down in reverse space
      } else {
        xEnd = vr[revIdx - 1] + 1; // right in reverse space
      }

      int64_t yEnd = xEnd - revK;
      int64_t x = xEnd;
      int64_t y = yEnd;
      while (x < n && y < m &&
             a[static_cast<size_t>(aHi - 1 - x)] ==
                 b[static_cast<size_t>(bHi - 1 - y)]) {
        ++x;
        ++y;
      }
      vr[revIdx] = x;

      if (!oddDelta) {
        const int64_t fwdK = delta - revK;
        if (fwdK >= -d && fwdK <= d &&
            vf[static_cast<size_t>(fwdK + offset)] + x >= n) {
          return MiddleSnake{aHi - x, bHi - y, aHi - xEnd, bHi - yEnd};
        }
      }
    }
  }

  REFOLD_LOG_FATAL("diff/myers", "internal: failed to find middle snake");
}

/// Emit a shortest edit script for one Myers divide-and-conquer subproblem.
///
/// This is the recursive linear-space Myers path. It trims equal prefix/suffix
/// runs from the current A/B box, handles empty-middle base cases directly, and
/// otherwise splits the remaining middle box at Myers' middle snake. Steps are
/// appended in forward order as recursion unwinds, so callers do not need a
/// reversal or normalization pass.
static void diffLinearRec(ArrayRef<StringRef> a, int64_t aLo, int64_t aHi,
                          ArrayRef<StringRef> b, int64_t bLo, int64_t bHi,
                          std::vector<Step> &out) {
  // Peel a common prefix eagerly so recursive boxes stay small and the emitted
  // script remains in forward order without any post-pass reversal.
  while (aLo < aHi && bLo < bHi &&
         a[static_cast<size_t>(aLo)] == b[static_cast<size_t>(bLo)]) {
    appendEqualSteps(out, static_cast<uint64_t>(aLo),
                     static_cast<uint64_t>(bLo), 1);
    ++aLo;
    ++bLo;
  }

  // Peel a common suffix, but emit it after the middle box has been processed
  // so the final SES still appears in forward order.
  int64_t suffixLen = 0;
  while (aLo + suffixLen < aHi && bLo + suffixLen < bHi &&
         a[static_cast<size_t>(aHi - 1 - suffixLen)] ==
             b[static_cast<size_t>(bHi - 1 - suffixLen)]) {
    ++suffixLen;
  }

  // Exclude the peeled suffix from the recursive middle box. The suffix is
  // emitted exactly once at each return site after the middle work is done.
  const int64_t aMidHi = aHi - suffixLen;
  const int64_t bMidHi = bHi - suffixLen;

  if (aLo == aMidHi && bLo == bMidHi) {
    appendEqualSteps(out, static_cast<uint64_t>(aMidHi),
                     static_cast<uint64_t>(bMidHi),
                     static_cast<uint64_t>(suffixLen));
    return;
  }

  // If the A side of the middle box is empty, the remaining B middle is a pure
  // insertion before the peeled suffix.
  if (aLo == aMidHi) {
    appendInsertSteps(out, static_cast<uint64_t>(aLo),
                      static_cast<uint64_t>(bLo),
                      static_cast<uint64_t>(bMidHi));
    appendEqualSteps(out, static_cast<uint64_t>(aMidHi),
                     static_cast<uint64_t>(bMidHi),
                     static_cast<uint64_t>(suffixLen));
    return;
  }

  // If the B side of the middle box is empty, the remaining A middle is a pure
  // deletion before the peeled suffix.
  if (bLo == bMidHi) {
    appendDeleteSteps(out, static_cast<uint64_t>(aLo),
                      static_cast<uint64_t>(aMidHi),
                      static_cast<uint64_t>(bLo));
    appendEqualSteps(out, static_cast<uint64_t>(aMidHi),
                     static_cast<uint64_t>(bMidHi),
                     static_cast<uint64_t>(suffixLen));
    return;
  }

  const MiddleSnake snake = findMiddleSnake(a, aLo, aMidHi, b, bLo, bMidHi);

  // Recurse around the middle snake. The snake itself is an equal run on an
  // optimal SES path, so it can be emitted directly between the two
  // subproblems.
  diffLinearRec(a, aLo, snake.aStart, b, bLo, snake.bStart, out);
  appendEqualSteps(out, static_cast<uint64_t>(snake.aStart),
                   static_cast<uint64_t>(snake.bStart),
                   static_cast<uint64_t>(snake.aEnd - snake.aStart));
  diffLinearRec(a, snake.aEnd, aMidHi, b, snake.bEnd, bMidHi, out);
  appendEqualSteps(out, static_cast<uint64_t>(aMidHi),
                   static_cast<uint64_t>(bMidHi),
                   static_cast<uint64_t>(suffixLen));
}
} // namespace

std::vector<Step> diff(ArrayRef<StringRef> a, ArrayRef<StringRef> b) {
  if (a.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
      b.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    REFOLD_LOG_FATAL("diff/myers", "input size exceeds int64_t range");
  }

  std::vector<Step> out;
  out.reserve(a.size() + b.size());
  diffLinearRec(a, 0, static_cast<int64_t>(a.size()), b, 0,
                static_cast<int64_t>(b.size()), out);
  return out;
}

std::vector<Hunk> coalesce(ArrayRef<Step> steps) {
  std::vector<Hunk> hunks;
  size_t i = 0;
  const size_t n = steps.size();

  while (i < n) {
    // 1. Skip over EQUAL steps.
    // These are the regions where A and B match perfectly.
    while (i < n && steps[i].op == Op::Equal) {
      ++i;
    }

    if (i >= n)
      break;

    // 2. Start of an edit region (a Hunk).
    // We capture the starting bounds from the first non-equal step.
    const uint64_t aStart = steps[i].aLo;
    const uint64_t bStart = steps[i].bLo;

    uint64_t aEnd = steps[i].aHi;
    uint64_t bEnd = steps[i].bHi;

    // 3. Consume all consecutive non-EQUAL steps.
    // This merges contiguous Inserts and Deletes into one Hunk.
    while (i < n && steps[i].op != Op::Equal) {
      aEnd = steps[i].aHi;
      bEnd = steps[i].bHi;
      ++i;
    }

    // 4. Create the Hunk.
    hunks.push_back(Hunk{aStart, aEnd, bStart, bEnd});
  }

  return hunks;
}

} // namespace diffutils
} // namespace refold
} // namespace clang

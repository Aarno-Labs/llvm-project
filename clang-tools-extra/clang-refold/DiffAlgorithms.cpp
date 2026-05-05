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
//     (typically token spellings), with a stable tie-breaker.
//   • Hunk construction from an A→B alignment (contiguous edit regions).
//   • Myers O((N+M)*D) shortest edit script (SES) with linear-space reconstruc-
//     tion.
//
// Responsibilities
// ----------------
//   • lcsMapAB: compute a one-sided mapping from indices in A to matching
//     indices in B (or -1 if unmatched), using a DP table with a deterministic
//     backtrack rule (on ties, advance in A).
//   • hunksFromMap: convert an A→B map into ordered edit hunks between anchors.
//   • myersDiff / coalesce: produce SES steps (EQUAL/INSERT/DELETE) and merge
//     adjacent non-EQUAL runs into hunks.
//
// Determinism & Policy
// --------------------
//   • All algorithms break ties consistently for stable output across runs.
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
//   • std::vector<int> lcsMapAB(...):
//       A[i] -> B[j] (j >= 0) or -1; deterministic tie-break.
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

#include "RefoldLog.h"
#include "DiffAlgorithms.h"
#include "StringUtils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <climits>
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
bool shouldUseGreedyApproach(unsigned long long n, unsigned long long m,
                             unsigned long long maxCells) {
  if (n >= std::numeric_limits<unsigned long long>::max() - 1ULL ||
      m >= std::numeric_limits<unsigned long long>::max() - 1ULL) {
    warn("lcs/map",
         "hirschberg fallback: (n+1) or (m+1) would overflow unsigned long long "
         "(n={0}, m={1})",
         n, m);
    return true; // (n+1) or (m+1) would overflow ULL anyway
  } else {
    const unsigned long long n1 = n + 1ULL;
    const unsigned long long m1 = m + 1ULL;

    // Product check via division to avoid overflow: n1 * m1 > maxCells ?
    const unsigned long long maxN1 = maxCells / m1; // m1 >= 1 always
    if (n1 > maxN1) {
      warn(
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
        warn("lcs/map",
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

inline bool isBetter(unsigned candLen, std::uint64_t candCost,
                     std::uint32_t candTie, unsigned bestLen,
                     std::uint64_t bestCost, std::uint32_t bestTie) {
  if (candLen != bestLen)
    return candLen > bestLen;
  if (candCost != bestCost)
    return candCost < bestCost;
  return candTie < bestTie;
}

// Small tie-break penalty for ambiguous matches.
//
// The weighted-LCS objective is (1) maximize LCS length, then (2) minimize
// ownerDepthGap cost. Those objectives implement the boundary policy.
//
// In many real-world streams, there can still be multiple optimal solutions
// with identical (len,cost), especially around highly repetitive tokens
// (punctuation, keywords, small literals). A naive backtrack that prefers the
// diagonal match can then "steal" from adjacent insertions by matching a token
// to the wrong repeated occurrence.
//
// We resolve this by introducing a tertiary objective dpTie that is only used
// when (len,cost) are equal. dpTie prefers matches whose immediate neighbors
// (prev/next) also match, biasing toward locally consistent alignments without
// changing optimality under (len,cost).
inline std::uint32_t
matchTiePenalty(ArrayRef<StringRef> a, ArrayRef<StringRef> b, size_t ai,
                size_t bj,
                const llvm::DenseMap<StringRef, unsigned> & /*freqA*/,
                const llvm::DenseMap<StringRef, unsigned> & /*freqB*/) {
  // Deterministic tertiary objective for tie-breaking among solutions with
  // identical (len,cost): prefer locally coherent alignments.
  //
  // Penalize a candidate match when its immediate neighbors disagree. This
  // biases toward choosing the occurrence that is consistent with surrounding
  // context, preventing optimal-but-pathological anchors that can absorb
  // nearby insertions.

  std::uint32_t p = 0;
  // Previous token agreement (bigram coherence).
  if (ai > 0 && bj > 0)
    p += (a[ai - 1] == b[bj - 1]) ? 0U : 1U;
  // Next token agreement (lookahead coherence).
  if (ai + 1 < a.size() && bj + 1 < b.size())
    p += (a[ai + 1] == b[bj + 1]) ? 0U : 1U;
  return p;
}

// ===================== Hirschberg (exact, linear space) ======================

struct Score {
  uint32_t len = 0;
  uint64_t cost = 0;
  uint32_t tie = 0;

  Score operator+(const Score &other) const {
    Score out;
    out.len = len + other.len;
    out.cost = cost + other.cost;
    out.tie = tie + other.tie;
    return out;
  }

  bool operator==(const Score &other) const {
    return len == other.len && cost == other.cost && tie == other.tie;
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

static std::vector<Score>
computeRowWeighted(const SpanView &aV, const SpanView &bV, const GapView &gapV,
                   ArrayRef<StringRef> aFull, ArrayRef<StringRef> bFull,
                   const llvm::DenseMap<StringRef, unsigned> &freqA,
                   const llvm::DenseMap<StringRef, unsigned> &freqB) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (gapV.size() != n + 1)
    fatal("lcs/map", "internal: gap view length must be A.len+1");

  std::vector<Score> dp(m + 1);
  std::vector<Score> ndp(m + 1);

  dp[0] = Score{0, 0, 0};
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
      best.tie = std::numeric_limits<uint32_t>::max();

      // 1) Match (diag)
      if (aV.at(i - 1) == bV.at(j - 1)) {
        Score cand = diagPrev;
        cand.len += 1U;
        cand.tie += matchTiePenalty(aFull, bFull, aV.absIndex(i - 1),
                                    bV.absIndex(j - 1), freqA, freqB);
        best = cand;
      }

      // 2) Delete A (from dp[j])
      {
        Score cand = dp[j];
        cand.cost += static_cast<uint64_t>(gapV.at(i));
        if (isBetter(cand.len, cand.cost, cand.tie, best.len, best.cost,
                     best.tie)) {
          best = cand;
        }
      }

      // 3) Insert B (from ndp[j-1])
      {
        Score cand = ndp[j - 1];
        cand.cost += static_cast<uint64_t>(gapV.at(i));
        if (isBetter(cand.len, cand.cost, cand.tie, best.len, best.cost,
                     best.tie)) {
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

static void
solveSmallWeightedDP(const SpanView &aV, const SpanView &bV,
                     const GapView &gapV, ArrayRef<StringRef> aFull,
                     ArrayRef<StringRef> bFull,
                     const llvm::DenseMap<StringRef, unsigned> &freqA,
                     const llvm::DenseMap<StringRef, unsigned> &freqB,
                     std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (gapV.size() != n + 1)
    fatal("lcs/map", "internal: gap view length must be A.len+1");

  const size_t stride = m + 1;
  const size_t cells = (n + 1) * (m + 1);

  std::vector<uint32_t> dpLen(cells, 0);
  std::vector<uint64_t> dpCost(cells, 0);
  std::vector<uint32_t> dpTie(cells, 0);

  auto idx = [&](size_t i, size_t j) -> size_t { return i * stride + j; };
  auto len = [&](size_t i, size_t j) -> uint32_t & { return dpLen[idx(i, j)]; };
  auto cost = [&](size_t i, size_t j) -> uint64_t & {
    return dpCost[idx(i, j)];
  };
  auto tie = [&](size_t i, size_t j) -> uint32_t & { return dpTie[idx(i, j)]; };

  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      if (i == 0 && j == 0)
        continue;

      uint32_t bestLen = 0;
      uint64_t bestCost = std::numeric_limits<uint64_t>::max();
      uint32_t bestTie = std::numeric_limits<uint32_t>::max();

      // Match
      if (i > 0 && j > 0 && aV.at(i - 1) == bV.at(j - 1)) {
        bestLen = len(i - 1, j - 1) + 1U;
        bestCost = cost(i - 1, j - 1);
        bestTie = tie(i - 1, j - 1) +
                  matchTiePenalty(aFull, bFull, aV.absIndex(i - 1),
                                  bV.absIndex(j - 1), freqA, freqB);
      }

      // Delete A (pay at boundary i)
      if (i > 0) {
        const uint32_t candLen = len(i - 1, j);
        const uint64_t candCost =
            cost(i - 1, j) + static_cast<uint64_t>(gapV.at(i));
        const uint32_t candTie = tie(i - 1, j);
        if (isBetter(candLen, candCost, candTie, bestLen, bestCost, bestTie)) {
          bestLen = candLen;
          bestCost = candCost;
          bestTie = candTie;
        }
      }

      // Insert B (pay at boundary i)
      if (j > 0) {
        const uint32_t candLen = len(i, j - 1);
        const uint64_t candCost =
            cost(i, j - 1) + static_cast<uint64_t>(gapV.at(i));
        const uint32_t candTie = tie(i, j - 1);
        if (isBetter(candLen, candCost, candTie, bestLen, bestCost, bestTie)) {
          bestLen = candLen;
          bestCost = candCost;
          bestTie = candTie;
        }
      }

      len(i, j) = bestLen;
      cost(i, j) = bestCost;
      tie(i, j) =
          (bestTie == std::numeric_limits<uint32_t>::max()) ? 0U : bestTie;
    }
  }

  // Backtrack (diag, then up, then left)
  size_t i = n;
  size_t j = m;
  while (i > 0 || j > 0) {
    const uint32_t curLen = len(i, j);
    const uint64_t curCost = cost(i, j);
    const uint32_t curTie = tie(i, j);

    bool moved = false;

    // Diagonal match
    if (i > 0 && j > 0 && aV.at(i - 1) == bV.at(j - 1)) {
      const uint32_t pen = matchTiePenalty(aFull, bFull, aV.absIndex(i - 1),
                                           bV.absIndex(j - 1), freqA, freqB);
      if (len(i - 1, j - 1) == curLen - 1U && cost(i - 1, j - 1) == curCost &&
          tie(i - 1, j - 1) + pen == curTie) {
        outMap[aV.absIndex(i - 1)] = static_cast<int64_t>(bV.absIndex(j - 1));
        --i;
        --j;
        moved = true;
      }
    }

    // Up (delete A)
    if (!moved && i > 0) {
      if (len(i - 1, j) == curLen &&
          cost(i - 1, j) + static_cast<uint64_t>(gapV.at(i)) == curCost &&
          tie(i - 1, j) == curTie) {
        --i;
        moved = true;
      }
    }

    // Left (insert B)
    if (!moved && j > 0) {
      if (len(i, j - 1) == curLen &&
          cost(i, j - 1) + static_cast<uint64_t>(gapV.at(i)) == curCost &&
          tie(i, j - 1) == curTie) {
        --j;
        moved = true;
      }
    }

    if (!moved)
      break;
  }
}

static void
hirschbergWeightedRec(const SpanView &aV, const SpanView &bV,
                      const GapView &gapV, ArrayRef<StringRef> aFull,
                      ArrayRef<StringRef> bFull,
                      const llvm::DenseMap<StringRef, unsigned> &freqA,
                      const llvm::DenseMap<StringRef, unsigned> &freqB,
                      std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (n == 0 || m == 0)
    return;

  // Small exact DP base case.
  const unsigned long long cells = static_cast<unsigned long long>(n + 1ULL) *
                                   static_cast<unsigned long long>(m + 1ULL);
  if (cells <= (1ULL << 20)) {
    solveSmallWeightedDP(aV, bV, gapV, aFull, bFull, freqA, freqB, outMap);
    return;
  }

  const size_t mid = n / 2;

  const SpanView aLeft{aV.base, aV.off, mid, aV.rev};
  const GapView gapLeft{gapV.base, gapV.off, mid + 1, gapV.rev};

  const SpanView aRight{aV.base, aV.off + mid, n - mid, aV.rev};
  const GapView gapRight{gapV.base, gapV.off + mid, (n - mid) + 1, gapV.rev};

  const std::vector<Score> leftRow =
      computeRowWeighted(aLeft, bV, gapLeft, aFull, bFull, freqA, freqB);

  const SpanView aRightRev{aV.base, aV.off + mid, n - mid, true};
  const GapView gapRightRev{gapV.base, gapV.off + mid, (n - mid) + 1, true};
  const SpanView bRev{bV.base, bV.off, m, true};

  const std::vector<Score> rightRowRev = computeRowWeighted(
      aRightRev, bRev, gapRightRev, aFull, bFull, freqA, freqB);

  // Choose split j maximizing combined (len, cost, tie). On exact equality,
  // prefer the smallest j for determinism.
  size_t bestJ = 0;
  Score best = leftRow[0] + rightRowRev[m];
  for (size_t j = 1; j <= m; ++j) {
    Score cand = leftRow[j] + rightRowRev[m - j];
    if (isBetter(cand.len, cand.cost, cand.tie, best.len, best.cost,
                 best.tie) || (cand == best && j < bestJ)) {
      bestJ = j;
      best = cand;
    }
  }

  const SpanView bLeft{bV.base, bV.off, bestJ, bV.rev};
  const SpanView bRight{bV.base, bV.off + bestJ, m - bestJ, bV.rev};

  hirschbergWeightedRec(aLeft, bLeft, gapLeft, aFull, bFull, freqA, freqB,
                        outMap);
  hirschbergWeightedRec(aRight, bRight, gapRight, aFull, bFull, freqA, freqB,
                        outMap);
}

static std::vector<int64_t>
lcsMapABHirschbergWeighted(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                           ArrayRef<uint32_t> ownerDepthGap,
                           const llvm::DenseMap<StringRef, unsigned> &freqA,
                           const llvm::DenseMap<StringRef, unsigned> &freqB) {
  std::vector<int64_t> out(a.size(), -1);
  const SpanView aV{a, 0, a.size(), false};
  const SpanView bV{b, 0, b.size(), false};
  const GapView gV{ownerDepthGap, 0, ownerDepthGap.size(), false};
  hirschbergWeightedRec(aV, bV, gV, a, b, freqA, freqB, out);
  return out;
}

// Unweighted Hirschberg (length-only) used for the non-policy overload.
static std::vector<unsigned> computeRowLen(const SpanView &aV,
                                           const SpanView &bV) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  std::vector<unsigned> dp(m + 1, 0);
  std::vector<unsigned> ndp(m + 1, 0);
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

static void solveSmallUnweightedDP(const SpanView &aV, const SpanView &bV,
                                   std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  const size_t stride = m + 1;
  const size_t cells = (n + 1) * (m + 1);
  std::vector<unsigned> dp(cells, 0);
  auto DP = [&](size_t i, size_t j) -> unsigned & {
    return dp[i * stride + j];
  };

  for (size_t i = n; i-- > 0;) {
    for (size_t j = m; j-- > 0;) {
      DP(i, j) = (aV.at(i) == bV.at(j)) ? DP(i + 1, j + 1) + 1U
                                        : std::max(DP(i + 1, j), DP(i, j + 1));
    }
  }

  size_t i = 0, j = 0;
  while (i < n && j < m) {
    if (aV.at(i) == bV.at(j)) {
      outMap[aV.absIndex(i)] = static_cast<int64_t>(bV.absIndex(j));
      ++i;
      ++j;
    } else if (DP(i + 1, j) >= DP(i, j + 1)) {
      ++i;
    } else {
      ++j;
    }
  }
}

static void hirschbergUnweightedRec(const SpanView &aV, const SpanView &bV,
                                    std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (n == 0 || m == 0)
    return;

  const unsigned long long cells = static_cast<unsigned long long>(n + 1ULL) *
                                   static_cast<unsigned long long>(m + 1ULL);
  if (cells <= (1ULL << 20)) {
    solveSmallUnweightedDP(aV, bV, outMap);
    return;
  }

  const size_t mid = n / 2;
  const SpanView aLeft{aV.base, aV.off, mid, aV.rev};
  const SpanView aRight{aV.base, aV.off + mid, n - mid, aV.rev};

  const std::vector<unsigned> leftRow = computeRowLen(aLeft, bV);
  const SpanView aRightRev{aV.base, aV.off + mid, n - mid, true};
  const SpanView bRev{bV.base, bV.off, m, true};
  const std::vector<unsigned> rightRowRev = computeRowLen(aRightRev, bRev);

  size_t bestJ = 0;
  unsigned bestLen = leftRow[0] + rightRowRev[m];
  for (size_t j = 1; j <= m; ++j) {
    const unsigned candLen = leftRow[j] + rightRowRev[m - j];
    if (candLen > bestLen || (candLen == bestLen && j < bestJ)) {
      bestLen = candLen;
      bestJ = j;
    }
  }

  const SpanView bLeft{bV.base, bV.off, bestJ, bV.rev};
  const SpanView bRight{bV.base, bV.off + bestJ, m - bestJ, bV.rev};

  hirschbergUnweightedRec(aLeft, bLeft, outMap);
  hirschbergUnweightedRec(aRight, bRight, outMap);
}

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

std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a,
                          ArrayRef<StringRef> b,
                          ArrayRef<uint32_t> ownerDepthGap,
                          unsigned long long maxCells) {
  using namespace clang::refold;

  const size_t n = a.size(), m = b.size();

  // Early outs for empties
  if (n == 0)
    return {};
  if (m == 0)
    return std::vector<int64_t>(n, -1);

  // Validate ownerDepthGap shape
  if (ownerDepthGap.size() != n + 1) {
    fatal("lcs/map", "ownerDepthGap length must be A.size() + 1");
  }

  // Indices are stored in int64_t; extremely large B streams are not
  // representable.
  if (m > MAX)
    fatal("lcs/map", "B.size() exceeds int64_t index range");

  // Build frequency maps once per call for ambiguity detection (also used by
  // Hirschberg).
  llvm::DenseMap<StringRef, unsigned> freqA;
  llvm::DenseMap<StringRef, unsigned> freqB;
  freqA.reserve(a.size());
  freqB.reserve(b.size());
  for (StringRef t : a)
    ++freqA[t];
  for (StringRef t : b)
    ++freqB[t];

  // DP table guard: if the full (n+1)*(m+1) table is too large, use Hirschberg
  // to remain exact while using only O(n+m) memory.
  const unsigned long long nu = static_cast<unsigned long long>(n);
  const unsigned long long mu = static_cast<unsigned long long>(m);
  const bool useHirschberg = shouldUseGreedyApproach(nu, mu, maxCells);
  if (useHirschberg)
    return lcsMapABHirschbergWeighted(a, b, ownerDepthGap, freqA, freqB);

  // ---------------- DP path: (n+1) x (m+1) tables, row-major -----------------
  // dpLen(i,j)  = max LCS length for A[0..i) vs B[0..j)
  // dpCost(i,j) = min accumulated gap cost among paths achieving dpLen(i,j)
  // dpTie(i,j)  = min tie-break penalty among paths achieving (dpLen, dpCost)
  //
  // Transitions into (i,j):
  //   match:     from (i-1,j-1), +1 length, +0 cost
  //   delete A:  from (i-1,j),   +0 length, +ownerDepthGap[i]
  //   insert B:  from (i,  j-1), +0 length, +ownerDepthGap[i]
  const size_t stride = m + 1;
  const size_t cells = (n + 1) * (m + 1);

  std::vector<uint32_t> dpLen(cells, 0);
  // Using uint64_t for cost to prevent overflow during accumulation
  std::vector<uint64_t> dpCost(cells, 0);
  std::vector<uint32_t> dpTie(cells, 0);

  auto idx = [&](size_t i, size_t j) -> size_t {
    return i * stride + j;
  };
  auto len = [&](size_t i, size_t j) -> uint32_t & {
    return dpLen[idx(i, j)];
  };
  auto cost = [&](size_t i, size_t j) -> uint64_t & {
    return dpCost[idx(i, j)];
  };
  auto tie = [&](size_t i, size_t j) -> uint32_t & {
    return dpTie[idx(i, j)];
  };

  // Fill DP table forward
  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      if (i == 0 && j == 0)
        continue;

      uint32_t bestLen = 0;
      uint64_t bestCost = std::numeric_limits<uint64_t>::max();
      uint32_t bestTie = std::numeric_limits<uint32_t>::max();

      // 1) Match (diagonal)
      if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
        bestLen = len(i - 1, j - 1) + 1U;
        bestCost = cost(i - 1, j - 1);
        bestTie = tie(i - 1, j - 1) +
                  matchTiePenalty(a, b, i - 1, j - 1, freqA, freqB);
      }

      // 2) Delete A (vertical move: i-1 -> i), pay ownerDepthGap[i]
      if (i > 0) {
        const uint32_t candLen = len(i - 1, j);
        const uint64_t candCost =
            cost(i - 1, j) + static_cast<uint64_t>(ownerDepthGap[i]);
        const uint32_t candTie = tie(i - 1, j);

        if (isBetter(candLen, candCost, candTie, bestLen, bestCost, bestTie)) {
          bestLen = candLen;
          bestCost = candCost;
          bestTie = candTie;
        }
      }

      // 3) Insert B (horizontal move: j-1 -> j), pay ownerDepthGap[i]
      if (j > 0) {
        const uint32_t candLen = len(i, j - 1);
        const uint64_t candCost =
            cost(i, j - 1) + static_cast<uint64_t>(ownerDepthGap[i]);
        const uint32_t candTie = tie(i, j - 1);

        if (isBetter(candLen, candCost, candTie, bestLen, bestCost, bestTie)) {
          bestLen = candLen;
          bestCost = candCost;
          bestTie = candTie;
        }
      }

      len(i, j) = bestLen;
      cost(i, j) = bestCost;
      tie(i, j) =
          (bestTie == std::numeric_limits<uint32_t>::max()) ? 0U : bestTie;
    }
  }

  // ------------------- Backtrack: diag, then up, then left -------------------
  std::vector<int64_t> map(n, -1);

  size_t i = n;
  size_t j = m;
  while (i > 0 || j > 0) {
    const uint32_t curLen = len(i, j);
    const uint64_t curCost = cost(i, j);
    const uint32_t curTie = tie(i, j);

    bool moved = false;

    // Diagonal (match)
    if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
      const uint32_t pen = matchTiePenalty(a, b, i - 1, j - 1, freqA, freqB);
      if (len(i - 1, j - 1) == curLen - 1U && cost(i - 1, j - 1) == curCost &&
          tie(i - 1, j - 1) + pen == curTie) {
        map[i - 1] = static_cast<int64_t>(j - 1);
        --i;
        --j;
        moved = true;
      }
    }

    // Up (delete A): from (i-1, j) paying ownerDepthGap[i]
    if (!moved && i > 0) {
      if (len(i - 1, j) == curLen &&
          cost(i - 1, j) + static_cast<uint64_t>(ownerDepthGap[i]) == curCost &&
          tie(i - 1, j) == curTie) {
        --i;
        moved = true;
      }
    }

    // Left (insert B): from (i, j-1) paying ownerDepthGap[i]
    if (!moved && j > 0) {
      if (len(i, j - 1) == curLen &&
          cost(i, j - 1) + static_cast<uint64_t>(ownerDepthGap[i]) == curCost &&
          tie(i, j - 1) == curTie) {
        --j;
        moved = true;
      }
    }

    if (!moved)
      break;
  }

  return map;
}

// ==================== LCS (DP with Hirschberg fallback) ======================

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
    fatal("lcs/map", "B.size() exceeds int64_t index range");

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

  auto DP = [&](size_t i, size_t j) -> unsigned & {
    return dp[i * stride + j];
  };

  // DP(i,j) = LCS length of a[i:] vs b[j:]
  for (size_t i = n; i-- > 0;) {
    for (size_t j = m; j-- > 0;) {
      DP(i, j) = (a[i] == b[j]) ? static_cast<unsigned>(DP(i + 1, j + 1) + 1U)
                                : std::max(DP(i + 1, j), DP(i, j + 1));
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
    } else if (DP(i + 1, j) >= DP(i, j + 1)) {
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

static void appendEqualSteps(std::vector<Step> &out, uint64_t aLo, uint64_t bLo,
                             uint64_t len) {
  for (uint64_t i = 0; i < len; ++i) {
    out.push_back(
        Step{Op::Equal, aLo + i, aLo + i + 1, bLo + i, bLo + i + 1});
  }
}

static void appendInsertSteps(std::vector<Step> &out, uint64_t aPos,
                              uint64_t bLo, uint64_t bHi) {
  for (uint64_t j = bLo; j < bHi; ++j)
    out.push_back(Step{Op::Insert, aPos, aPos, j, j + 1});
}

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

  fatal("diff/myers", "internal: failed to find middle snake");
}

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

  const int64_t aMidHi = aHi - suffixLen;
  const int64_t bMidHi = bHi - suffixLen;

  if (aLo == aMidHi && bLo == bMidHi) {
    appendEqualSteps(out, static_cast<uint64_t>(aMidHi),
                     static_cast<uint64_t>(bMidHi),
                     static_cast<uint64_t>(suffixLen));
    return;
  }

  if (aLo == aMidHi) {
    appendInsertSteps(out, static_cast<uint64_t>(aLo),
                      static_cast<uint64_t>(bLo),
                      static_cast<uint64_t>(bMidHi));
    appendEqualSteps(out, static_cast<uint64_t>(aMidHi),
                     static_cast<uint64_t>(bMidHi),
                     static_cast<uint64_t>(suffixLen));
    return;
  }

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
    fatal("diff/myers", "input size exceeds int64_t range");
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

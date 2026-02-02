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
//   • Myers O(N*D) shortest edit script (SES) with coalescing helpers.
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
//   • Large-input guard: LCS may fall back to a greedy, order-preserving
//     subsequence when the DP table would exceed a configured cell budget.
//   • Utilities are side-effect free and operate on caller-owned sequences.
//
// Complexity
// ----------
//   • LCS DP:   time O(N*M), space O(N*M); greedy fallback O(N+M).
//   • Myers:    expected time O((N+M)*D), space O(N+M) per frontier snapshot.
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
         "greedy fallback: (n+1) or (m+1) would overflow unsigned long long "
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
          "greedy fallback: DP cell budget exceeded: (n+1)*(m+1) > maxCells "
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
             "greedy fallback: DP allocation would overflow size_t for "
             "unsigned table "
             "(cells={0} > size_t/sizeof(unsigned)={1}; n={2}, m={3})",
             cells, cellLimit, n, m);
        return true;
      }
    }
  }
  return false;
}

std::vector<int64_t> lcsMapABGreedy(ArrayRef<StringRef> a,
                                ArrayRef<StringRef> b) {
  const size_t n = a.size(), m = b.size();

  // Greedy order-preserving subsequence scan (linear-time).
  // If m > MAX, only consider the first MAX elements of b so that
  // the stored j indices always fit in 'int'.
  const size_t jlimit = std::min(m, MAX);
  std::vector<int64_t> map(n, -1);
  size_t j = 0;
  for (size_t i = 0; i < n && j < jlimit; ++i) {
    while (j < jlimit && a[i] != b[j])
      ++j;
    if (j < jlimit && a[i] == b[j]) {
      map[i] = static_cast<int64_t>(j);
      ++j;
    }
  }
  return map;
}

inline bool isBetter(unsigned candLen, std::uint64_t candCost, unsigned bestLen,
                     std::uint64_t bestCost) {
  return (candLen > bestLen) || (candLen == bestLen && candCost < bestCost);
}
} // namespace

// ================== Weighted LCS (DP with greedy fallback) ===================

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

  // 1) If b is too large to store j in 'int' safely, prefer greedy.
  bool useGreedy = (m > MAX);

  // 2) Overflow-safe DP cell budget guard + allocation guards.
  const unsigned long long nu = static_cast<unsigned long long>(n);
  const unsigned long long mu = static_cast<unsigned long long>(m);
  if (!useGreedy) {
    useGreedy = shouldUseGreedyApproach(nu, mu, maxCells);
  }

  if (useGreedy) {
    return lcsMapABGreedy(a, b);
  }

  // Validate ownerDepthGap shape
  if (ownerDepthGap.size() != n + 1) {
    fatal("lcs/map", "ownerDepthGap length must be A.size() + 1");
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
  // Using uint64_t for cost to prevent overflow during accumulation
  std::vector<uint64_t> dpCost(cells, 0);

  auto idx = [&](size_t i, size_t j) -> size_t {
    return i * stride + j;
  };
  auto len = [&](size_t i, size_t j) -> uint32_t & {
    return dpLen[idx(i, j)];
  };
  auto cost = [&](size_t i, size_t j) -> uint64_t & {
    return dpCost[idx(i, j)];
  };

  // Fill DP table forward
  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      if (i == 0 && j == 0)
        continue;

      uint32_t bestLen = 0;
      uint64_t bestCost = std::numeric_limits<uint64_t>::max();

      // 1) Match (diagonal)
      if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
        bestLen = len(i - 1, j - 1) + 1U;
        bestCost = cost(i - 1, j - 1);
      }

      // 2) Delete A (vertical move: i-1 -> i), pay ownerDepthGap[i]
      if (i > 0) {
        const uint32_t candLen = len(i - 1, j);
        const uint64_t candCost =
            cost(i - 1, j) + static_cast<uint64_t>(ownerDepthGap[i]);

        if (isBetter(candLen, candCost, bestLen, bestCost)) {
          bestLen = candLen;
          bestCost = candCost;
        }
      }

      // 3) Insert B (horizontal move: j-1 -> j), pay ownerDepthGap[i]
      if (j > 0) {
        const uint32_t candLen = len(i, j - 1);
        const uint64_t candCost =
            cost(i, j - 1) + static_cast<uint64_t>(ownerDepthGap[i]);

        if (isBetter(candLen, candCost, bestLen, bestCost)) {
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

    // Diagonal (match) first
    if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
      if (len(i - 1, j - 1) == curLen - 1U &&
          cost(i - 1, j - 1) == curCost) {
        map[i - 1] = static_cast<int64_t>(j - 1);
        --i;
        --j;
        moved = true;
      }
    }

    // Up (delete A): from (i-1, j) paying ownerDepthGap[i]
    if (!moved && i > 0) {
      if (len(i - 1, j) == curLen &&
          cost(i - 1, j) + static_cast<uint64_t>(ownerDepthGap[i]) == curCost) {
        --i;
        moved = true;
      }
    }

    // Left (insert B): from (i, j-1) paying ownerDepthGap[i]
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

// ====================== LCS (DP with greedy fallback) =======================

[[maybe_unused]]
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                          unsigned long long maxCells) {
  const size_t n = a.size(), m = b.size();

  // Early outs for empties
  if (n == 0)
    return {};
  if (m == 0)
    return std::vector<int64_t>(n, -1);

  // Decide whether to use the greedy subsequence fallback.
  bool useGreedy = false;

  // 1) If b is too large to store j in 'int' safely, prefer greedy.
  if (m > MAX)
    useGreedy = true;

  // 2) Overflow-safe DP cell budget guard.
  const unsigned long long nu = static_cast<unsigned long long>(n);
  const unsigned long long mu = static_cast<unsigned long long>(m);

  if (!useGreedy) {
    useGreedy = shouldUseGreedyApproach(nu, mu, maxCells);
  }

  if (useGreedy) {
    return lcsMapABGreedy(a, b);
  }

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

// ============================= Myers O(n*d) diff =============================

/// \brief Reconstruct the forward-ordered sequence of Step edits from saved
/// frontiers.
///
/// Given the `trace` of frontier arrays `V` captured after each edit distance
/// layer `d`, and the terminal coordinates `(x, y)` at minimal edit distance
/// `dAtEnd`, this function walks backward from `d = dAtEnd` to `0`, choosing
/// the predecessor diagonal.
///
/// * If `V[k-1] < V[k+1]`, the path came from **down**
///   (`prevK = k+1`, an INSERT in B).
/// * Otherwise, it came from **right**
///   (`prevK = k-1`, a DELETE in A).
///
/// After choosing `prevK`, the algorithm emits any diagonal moves as `EQUAL`
/// steps, then emits the single edit step that transitioned to `(x, y)`. The
/// resulting list of `Step` edits is reversed at the end to obtain forward
/// order.
///
/// \param a Left sequence (used for equality comparisons).
/// \param b Right sequence (used for equality comparisons).
/// \param trace List of `V` snapshots (`trace[d-1]` corresponds to the
///              frontier before layer `d`).
/// \param x Terminal x-coordinate at the end of the SES.
/// \param y Terminal y-coordinate at the end of the SES.
/// \param dAtEnd Minimal edit distance at the end of the forward pass.
/// \param offset Offset used to index `V` by `k + offset`.
/// \returns Forward-ordered list of `Step` edits.
static std::vector<Step> backtrack(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                                   ArrayRef<std::vector<int64_t>> trace,
                                   int64_t x, int64_t y, int64_t dAtEnd,
                                   int64_t offset) {
  std::vector<Step> out;
  for (int64_t d = dAtEnd; d > 0; --d) {
    // trace contains snapshots of V arrays at each distance d
    const std::vector<int64_t> &v = trace[static_cast<size_t>(d - 1)];
    const int64_t k = x - y;
    const size_t kIndex = static_cast<size_t>(k + offset);

    // Choose predecessor diagonal
    int64_t prevK;
    // k == -d means we must have come from above (k+1)
    // Otherwise, we check if moving from k+1 or k-1 resulted in a further X
    if (k == -d || (k != d && v[kIndex - 1] < v[kIndex + 1])) {
      prevK = k + 1; // Vertical move (Insert)
    } else {
      prevK = k - 1; // Horizontal move (Delete)
    }

    const int64_t xStart = v[static_cast<size_t>(prevK + offset)];
    const int64_t yStart = xStart - prevK;

    // Diagonal snake: backtrack through equals
    while (x > xStart && y > yStart) {
      --x;
      --y;
      out.push_back(Step{Op::Equal, static_cast<uint64_t>(x),
                         static_cast<uint64_t>(x + 1), static_cast<uint64_t>(y),
                         static_cast<uint64_t>(y + 1)});
    }

    // Edit step: the move that increased distance d-1 to d
    if (xStart < x) {
      --x;
      out.push_back(Step{Op::Delete, static_cast<uint64_t>(x),
                         static_cast<uint64_t>(x + 1), static_cast<uint64_t>(y),
                         static_cast<uint64_t>(y)});
    } else if (yStart < y) {
      --y;
      out.push_back(Step{Op::Insert, static_cast<uint64_t>(x),
                         static_cast<uint64_t>(x), static_cast<uint64_t>(y),
                         static_cast<uint64_t>(y + 1)});
    }
  }

  // Leading equals before the first edit
  while (x > 0 && y > 0 &&
         a[static_cast<size_t>(x - 1)] == b[static_cast<size_t>(y - 1)]) {
    --x;
    --y;
    out.push_back(Step{Op::Equal, static_cast<uint64_t>(x),
                       static_cast<uint64_t>(x + 1), static_cast<uint64_t>(y),
                       static_cast<uint64_t>(y + 1)});
  }

  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<Step> diff(ArrayRef<StringRef> a, ArrayRef<StringRef> b) {
  const int64_t n = static_cast<int64_t>(a.size());
  const int64_t m = static_cast<int64_t>(b.size());
  const int64_t max = n + m;
  const int64_t offset = max;

  // V array tracks the furthest x reached on each diagonal k
  // Range is [-max, max], so size is 2*max + 1
  std::vector<int64_t> v(static_cast<size_t>(2 * max + 1), -1);

  // Base case: starting point (0,0) on diagonal 0
  v[static_cast<size_t>(offset + 1)] = 0;

  std::vector<std::vector<int64_t>> trace;
  // Reserve based on expected distance;
  // for very large diffs, this is the main memory consumer.
  trace.reserve(static_cast<size_t>(max + 1));

  for (int64_t d = 0; d <= max; ++d) {
    std::vector<int64_t> vNew = v;

    for (int64_t k = -d; k <= d; k += 2) {
      const size_t kIdx = static_cast<size_t>(k + offset);

      int64_t x;
      if (k == -d || (k != d && v[kIdx - 1] < v[kIdx + 1])) {
        x = v[kIdx + 1]; // Move down from k+1 (Insertion)
      } else {
        x = v[kIdx - 1] + 1; // Move right from k-1 (Deletion)
      }

      int64_t y = x - k;

      // Greedy snake (Diagongal)
      while (x < n && y < m &&
             a[static_cast<size_t>(x)] == b[static_cast<size_t>(y)]) {
        x++;
        y++;
      }

      vNew[kIdx] = x;

      if (x >= n && y >= m) {
        trace.push_back(std::move(vNew));
        return backtrack(a, b, trace, x, y, d, offset);
      }
    }

    trace.push_back(std::move(vNew));
    v = trace.back();
  }

  return backtrack(a, b, trace, n, m, max, offset);
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

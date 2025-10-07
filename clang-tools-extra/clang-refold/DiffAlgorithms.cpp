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

#include "DiffAlgorithms.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <vector>

namespace clang {
namespace refold {
namespace diffutils {

// ========================= LCS (DP with greedy fallback)
// =========================

std::vector<int> lcsMapAB(const std::vector<std::string> &a,
                          const std::vector<std::string> &b,
                          unsigned long long maxCells) {
  const std::size_t n = a.size(), m = b.size();

  // Early outs for empties
  if (n == 0)
    return {};
  if (m == 0)
    return std::vector<int>(n, -1);

  // Decide whether to use the greedy subsequence fallback.
  bool useGreedy = false;

  // 1) If b is too large to store j in 'int' safely, prefer greedy.
  constexpr std::size_t MAX = std::numeric_limits<int>::max();
  if (m > MAX)
    useGreedy = true;

  // 2) Overflow-safe DP cell budget guard.
  const unsigned long long nu = static_cast<unsigned long long>(n);
  const unsigned long long mu = static_cast<unsigned long long>(m);

  if (!useGreedy) {
    if (nu >= std::numeric_limits<unsigned long long>::max() - 1ULL ||
        mu >= std::numeric_limits<unsigned long long>::max() - 1ULL) {
      useGreedy = true; // (n+1) or (m+1) would overflow ULL anyway
    } else {
      const unsigned long long nu1 = nu + 1ULL;
      const unsigned long long mu1 = mu + 1ULL;
      // Product check via division to avoid overflow: nu1 * mu1 > maxCells ?
      if (nu1 > maxCells / mu1) {
        useGreedy = true;
      } else {
        // 3) Allocation guard: cells * sizeof(unsigned) must fit in std::size_t
        const unsigned long long cells = nu1 * mu1;
        const unsigned long long cellLimit = static_cast<unsigned long long>(
            std::numeric_limits<std::size_t>::max() / sizeof(unsigned));
        if (cells > cellLimit)
          useGreedy = true;
      }
    }
  }

  if (useGreedy) {
    // Greedy order-preserving subsequence scan (linear-time).
    // If m > MAX, only consider the first MAX elements of b so that
    // the stored j indices always fit in 'int'.
    const std::size_t jlimit = std::min(m, MAX);
    std::vector<int> map(n, -1);
    std::size_t j = 0;
    for (std::size_t i = 0; i < n && j < jlimit; ++i) {
      while (j < jlimit && a[i] != b[j])
        ++j;
      if (j < jlimit && a[i] == b[j]) {
        map[i] = static_cast<int>(j);
        ++j;
      }
    }
    return map;
  }

  // ----------------- DP path: (n+1) x (m+1) table, row-major -----------------
  const std::size_t stride = m + 1;
  const std::size_t cells = (n + 1) * (m + 1);
  std::vector<unsigned> dp(cells, 0);

  auto DP = [&](std::size_t i, std::size_t j) -> unsigned & {
    return dp[i * stride + j];
  };

  // DP(i,j) = LCS length of a[i:] vs b[j:]
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      DP(i, j) = (a[i] == b[j]) ? static_cast<unsigned>(DP(i + 1, j + 1) + 1U)
                                : std::max(DP(i + 1, j), DP(i, j + 1));
    }
  }

  // Reconstruct a->b map with deterministic tie-breaker (>= → skip a)
  std::vector<int> map(n, -1);
  std::size_t i = 0, j = 0;
  while (i < n && j < m) {
    if (a[i] == b[j]) {
      map[i] = static_cast<int>(j);
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

std::vector<Hunk> hunksFromMap(const std::vector<int> &map, int nA, int nB) {
  std::vector<Hunk> hunks;
  int prevI = -1, prevJ = -1;
  for (int i = 0; i < nA; ++i) {
    const int j = (i < static_cast<int>(map.size()) ? map[i] : -1);
    if (j < 0)
      continue; // unmatched
    const int aStart = prevI + 1, aEnd = i;
    const int bStart = prevJ + 1, bEnd = j;
    if (aStart < aEnd || bStart < bEnd)
      hunks.push_back(Hunk{aStart, aEnd, bStart, bEnd});
    prevI = i;
    prevJ = j;
  }
  // tail region after last match
  const int aStart = prevI + 1, aEnd = nA;
  const int bStart = prevJ + 1, bEnd = nB;
  if (aStart < aEnd || bStart < bEnd)
    hunks.push_back(Hunk{aStart, aEnd, bStart, bEnd});
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
static std::vector<Step> backtrack(const std::vector<std::string> &a,
                                   const std::vector<std::string> &b,
                                   const std::vector<std::vector<int>> &trace,
                                   int x, int y, int dAtEnd, int offset) {
  std::vector<Step> out;
  for (int d = dAtEnd; d > 0; --d) {
    const std::vector<int> &v =
        trace[static_cast<std::size_t>(d - 1)]; // snapshot before layer d
    const int k = x - y;
    const int kIndex = k + offset;

    // Choose predecessor diagonal
    int prevK;
    if (k == -d || (k != d && v[static_cast<std::size_t>(kIndex - 1)] <
                                  v[static_cast<std::size_t>(kIndex + 1)])) {
      prevK = k + 1; // came from "down" (insert in b)
    } else {
      prevK = k - 1; // came from "right" (delete from a)
    }

    const int xStart = v[static_cast<std::size_t>(prevK + offset)];
    const int yStart = xStart - prevK;

    // Diagonal snake (equals)
    while (x > xStart && y > yStart) {
      --x;
      --y;
      out.push_back(Step{Op::Equal, x, x + 1, y, y + 1});
    }

    // Single edit step
    if (xStart < x) {
      --x;
      out.push_back(Step{Op::Delete, x, x + 1, y, y});
    } else if (yStart < y) {
      --y;
      out.push_back(Step{Op::Insert, x, x, y, y + 1});
    } else {
      // no-op
    }
  }

  // Any leading equals before d=0
  while (x > 0 && y > 0 &&
         a[static_cast<std::size_t>(x - 1)] ==
             b[static_cast<std::size_t>(y - 1)]) {
    --x;
    --y;
    out.push_back(Step{Op::Equal, x, x + 1, y, y + 1});
  }

  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<Step> diff(const std::vector<std::string> &a,
                       const std::vector<std::string> &b) {
  const int n = static_cast<int>(a.size());
  const int m = static_cast<int>(b.size());
  const int max = n + m;
  const int offset = max;

  // Trace of v snapshots for each d (we store vectors for simplicity).
  std::vector<std::vector<int>> trace;
  trace.reserve(static_cast<std::size_t>(max + 1));

  std::vector<int> v(static_cast<std::size_t>(2 * max + 1), 0);

  // Forward pass
  for (int d = 0; d <= max; ++d) {
    trace.push_back(v); // snapshot BEFORE processing layer d

    for (int k = -d; k <= d; k += 2) {
      const int kIndex = k + offset;

      int x;
      if (k == -d || (k != d && v[static_cast<std::size_t>(kIndex - 1)] <
                                    v[static_cast<std::size_t>(kIndex + 1)])) {
        // down: insert in a (advance in b)
        x = v[static_cast<std::size_t>(kIndex + 1)];
      } else {
        // right: delete from A (advance in A)
        x = v[static_cast<std::size_t>(kIndex - 1)] + 1;
      }
      int y = x - k;

      // Follow diagonal (snake)
      while (x < n && y < m &&
             a[static_cast<std::size_t>(x)] == b[static_cast<std::size_t>(y)]) {
        ++x;
        ++y;
      }
      v[static_cast<std::size_t>(kIndex)] = x;

      if (x >= n && y >= m) {
        // Reached end; backtrack to produce SES.
        return backtrack(a, b, trace, x, y, d, offset);
      }
    }
  }

  // Should be unreachable for finite inputs.
  llvm_unreachable("diff failed to reach end");
}

std::vector<Hunk> coalesce(const std::vector<Step> &steps) {
  std::vector<Hunk> hunks;
  std::size_t i = 0;
  const std::size_t n = steps.size();

  while (i < n) {
    // Skip equals
    while (i < n && steps[i].op == Op::Equal)
      ++i;
    if (i >= n)
      break;

    // Begin an edit run
    int aStart = steps[i].aLo;
    int bStart = steps[i].bLo;
    int aEnd = aStart, bEnd = bStart;

    // Consume consecutive non-EQUAL steps
    while (i < n && steps[i].op != Op::Equal) {
      aEnd = steps[i].aHi;
      bEnd = steps[i].bHi;
      ++i;
    }
    hunks.push_back(Hunk{aStart, aEnd, bStart, bEnd});
  }
  return hunks;
}

} // namespace diffutils
} // namespace refold
} // namespace clang

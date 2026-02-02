//===--- DiffAlgorithms.h ---------------------------------------*- C++ -*-===//
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
//                                    size_t nA, size_t nB):
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

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_DIFFALGORITHMS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_DIFFALGORITHMS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace diffutils {

constexpr unsigned long long DEFAULT_MAX_CELLS = 1ULL << 30;

// ===== Myers shortest edit script (SES) =====

enum class Op { Equal, Insert, Delete };

static inline StringRef toString(Op op) {
  switch (op) {
  case Op::Equal:
    return "Equal";
  case Op::Insert:
    return "Insert";
  case Op::Delete:
    return "Delete";
  }
  llvm_unreachable("Invalid op");
}

/// \brief Immutable atom in the shortest edit script (SES).
///
/// Each `Step` represents one maximal run of a single edit operation over
/// half-open index ranges in the left (`A`) and right (`B`) sequences:
/// `A[aLo,aHi)` and `B[bLo,bHi)`.
///
/// ### Semantics & invariants
/// * **EQUAL** — consumes elements from both sides:
///   * `aHi > aLo`, `bHi > bLo`
///   * Length equality holds: `(aHi - aLo) == (bHi - bLo)`
/// * **INSERT** — inserts `B[bLo,bHi)` with no elements consumed from `A`:
///   * `aHi == aLo`, `bHi > bLo`
/// * **DELETE** — deletes `A[aLo,aHi)` with no elements consumed from `B`:
///   * `aHi > aLo`, `bHi == bLo`
///
/// Steps are emitted in order, non-overlapping, and collectively transform
/// `A` into `B`. Replacements are represented as an adjacent **DELETE**
/// followed by **INSERT** (no separate REPLACE operation).
///
/// ### Notes
/// * Indices are zero-based; ranges are half-open `[lo,hi)`.
/// * “Maximal run” means contiguous operations of the same kind are
///   coalesced into a single `Step`.
struct Step {
  Op op;
  uint64_t aLo, aHi; // indices in A
  uint64_t bLo, bHi; // indices in B

  std::string ToString() const {
    std::string opStr;
    switch (op) {
    case Op::Equal:
      opStr = "EQUAL";
      break;
    case Op::Insert:
      opStr = "INSERT";
      break;
    case Op::Delete:
      opStr = "DELETE";
      break;
    }
    return formatv("{0} A[{1},{2}) -> B[{3},{4})", opStr, aLo, aHi, bLo, bHi);
  }
};

/// \brief Coalesced edit region spanning contiguous non-EQUAL steps in the SES.
///
/// A `Hunk` summarizes one contiguous *edit run* between the original
/// sequence `A` and the new sequence `B`:
/// `A[aStart,aEnd)` is replaced by `B[bStart,bEnd)`.
///
/// **Special cases:**
/// * **INSERT:** `aStart == aEnd && bStart < bEnd`
/// * **DELETE:** `aStart < aEnd && bStart == bEnd`
/// * **REPLACE:** `aStart < aEnd && bStart < bEnd`
/// * **EQUAL (degenerate):** `aStart == aEnd && bStart == bEnd`
///   *(Not normally emitted by `coalesce()`.)*
///
/// All indices are half-open and refer to element positions in the diff
/// input sequences (not byte offsets).
/// Instances are immutable and safe to reuse across passes.
struct Hunk {
  uint64_t aStart, aEnd;
  uint64_t bStart, bEnd;

  bool isInsertOnly() const { return (aStart == aEnd) && (bStart < bEnd); }
  bool isDeleteOnly() const { return (aStart < aEnd) && (bStart == bEnd); }
  bool isReplace() const { return (aStart < aEnd) && (bStart < bEnd); }
  bool isEqual() const { return (aStart == aEnd) && (bStart == bEnd); }

  template <bool kVerbose = false> std::string ToString() const {
    if (kVerbose) {
      std::string kind;
      if (isInsertOnly()) {
        kind.assign("INS");
      } else if (isDeleteOnly()) {
        kind.assign("DEL");
      } else if (isReplace()) {
        kind.assign("REP");
      } else if (isEqual()) {
        kind.assign("EQL");
      }
      assert(!kind.empty() && "invalid hunk");
      return formatv("{0} HUNK A[{1},{2}) -> B[{3},{4})", kind, aStart, aEnd,
                     bStart, bEnd);
    } else {
      return formatv("HUNK A[{0},{1}) -> B[{2},{3})", aStart, aEnd, bStart,
                     bEnd);
    }
  }
};

/// \brief Compute the shortest edit script (SES) between sequences A and B.
///
/// Computes the shortest edit script (SES) between sequences `A` and `B`
/// using Myers’ *O(N·D)* algorithm, returning the edit steps in forward order.
///
/// The algorithm performs a forward pass over increasing edit distance `d`,
/// maintaining the furthest-reaching frontier `V[k]` for each diagonal
/// `k = x - y`, and snapshots each frontier to `trace` for later
/// reconstruction. When the ends of both sequences are reached, the edit path
/// is reconstructed by `backtrack(trace, A, B, ...)`.
///
/// **Complexity:**
/// * Time: *O((N+M)·D)*
/// * Space: *O(N+M)* per frontier snapshot, *O(D·(N+M))* total for the trace.
/// Here `N = A.size()`, `M = B.size()`, and `D` is the minimal edit distance.
///
/// \param a Left sequence.
/// \param b Right sequence.
/// \returns Ordered list of `Step` records: `EQUAL`, `INSERT`, and `DELETE`.
std::vector<Step> diff(ArrayRef<StringRef> a, ArrayRef<StringRef> b);

/// \brief Coalesce contiguous non-EQUAL steps into larger Hunk regions.
///
/// Groups contiguous non-`EQUAL` steps in the shortest edit script (SES)
/// into coalesced `Hunk` regions.
///
/// The input is the raw SES as produced by `diff(A, B)`. This routine merges
/// runs of `INSERT`/`DELETE` (and their alternating combinations, which
/// together form a REPLACE) into a single `Hunk` per run, skipping isolated
/// `EQUAL` steps entirely. The result is a more convenient representation
/// for downstream refolding or diff processing.
///
/// \param steps SES steps from `diff(A, B)` in forward order.
/// \returns List of `Hunk` objects covering each contiguous edit run.
std::vector<Hunk> coalesce(ArrayRef<Step> steps);

// ========================== LCS alignment utilities ==========================

/// \brief Compute an owner-aware LCS backmap from sequence A to B.
///
/// Computes a one-sided longest common subsequence (LCS) mapping from sequence
/// `A` to `B` that respects the structural ownership of tokens. Unlike a
/// standard greedy LCS, this algorithm penalizes or prohibits alignments
/// between tokens that belong to different logical owners (e.g., different
/// include files or macro expansions).
///
/// Returns an array `map` of length `A.size()` where `map[i] = j` if `A[i]`
/// participates in a valid alignment with `B[j]`, or `-1` if `A[i]` is
/// unmatched.
///
/// ### Structural Constraints (Owner-Awareness)
///
/// The `ownerDepthGap` parameter provides the hierarchical "cost" of aligning
/// tokens at specific indices. The algorithm uses this to ensure that tokens
/// stay within their respective boundaries, preventing the "drift" where a
/// token in a header is mistakenly aligned with an identical-looking token
/// in the main TU.
///
/// ### Tie-breaker and Stability
///
/// In the event of equal LCS scores, the algorithm prefers advancing in `A`
/// (skipping `A[i]`) to maintain a stable, deterministic bias toward earlier
/// indices in `B`. This ensures that edits are projected back to the most
/// conservative possible locations in the original source.
///
/// ### Performance & Scaling
///
/// - **DP Path:** Used for small-to-medium sequences where the product of
///   lengths is less than \p maxCells. Employs a cost-model-augmented
///   Dynamic Programming approach.
/// - **Greedy Fallback:** If \p maxCells is exceeded, the algorithm falls back
///   to a linear-time scan that respects owner boundaries but may produce
///   a non-maximal subsequence.
///
/// ### Complexity
///
/// * **Time:** O(N*M) for DP, O(N+M) for fallback.
/// * **Space:** O(N*M) for the full DP table, O(N) for the result map.
///
/// \param a The original (Source A) sequence of tokens/strings.
/// \param b The edited (Source B) sequence of tokens/strings.
/// \param ownerDepthGap A parallel array to \p a indicating the ownership
///        depth or boundary cost for each token.
/// \param maxCells The threshold for the DP table size (N*M) before falling
///        back to a linear greedy scan.
/// \returns A vector mapping each index in \p a to its corresponding index
///          in \p b, or -1 if the token was deleted or moved.
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a,
                          ArrayRef<StringRef> b,
                          ArrayRef<uint32_t> ownerDepthGap,
                          unsigned long long maxCells = DEFAULT_MAX_CELLS);

/// \brief Compute a one-sided LCS backmap from sequence A to B using DP.
///
/// Computes a one-sided longest common subsequence (LCS) mapping from
/// sequence `A` to `B` with a deterministic tie-breaker.
///
/// Returns an array `map` of length `A.size()` where `map[i] = j` if
/// `A[i]` participates in an LCS alignment with `B[j]` (order-preserving),
/// or `-1` if `A[i]` is unmatched.
///
/// ### Tie-breaker (determinism)
///
/// When backtracking the DP table at a mismatch and both candidate
/// continuations have equal score, this implementation advances in `A`
/// (prefers `(i+1, j)` over `(i, j+1)`). In other words, on ties it
/// *skips `A[i]`* rather than `B[j]`.
///
/// **Effect:** Produces a stable alignment that biases matches toward
/// earlier indices in `B` and removes ambiguity among multiple optimal
/// LCS paths.
///
/// ### Large-input guard
///
/// If the full DP table would exceed a fixed cell budget (~20M cells),
/// the algorithm falls back to a greedy, order-preserving subsequence
/// scan: it advances a pointer through `B` and records the first position
/// where each `A[i]` equals `B[j]`. This fallback runs in linear time and
/// is deterministic, but may produce a non-maximal subsequence when DP is
/// skipped.
///
/// ### Complexity
///
/// * DP path: time *O(N×M)*, space *O(N×M)*
/// * Greedy fallback: time *O(N+M)*, space *O(N)*
///
/// \param a Left sequence.
/// \param b Right sequence.
/// \param maxCells Maximum number of cells before performing a greedy scan.
/// \returns A vector mapping a-indices to b-indices (or -1 if unmatched).
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                          unsigned long long maxCells = DEFAULT_MAX_CELLS);

/// \brief Convert an A→B alignment map into a list of edit hunks.
///
/// Converts a one-sided alignment map from sequence A to B into a list of
/// contiguous edit regions ("hunks") representing gaps between matched
/// anchors.
///
/// The input map has length `nA` and is interpreted as:
/// * `map[i] = j` → element `A[i]` is aligned to `B[j]`, and the sequence
///   of mapped `j` values over increasing `i` must be strictly increasing
///   (one-sided LCS-style alignment).
/// * `map[i] = -1` → `A[i]` is unmatched.
///
/// The function walks the aligned pairs in order and emits a `Hunk` for
/// every region between consecutive anchors, including the leading region
/// before the first match and the trailing region after the last match.
///
/// Each hunk describes the half-open intervals `A[aStart,aEnd)` and
/// `B[bStart,bEnd)` that differ:
/// * **INSERT:** `aStart == aEnd && bStart < bEnd`
/// * **DELETE:** `aStart < aEnd && bStart == bEnd`
/// * **REPLACE:** `aStart < aEnd && bStart < bEnd`
///
/// **Determinism:**
/// Output depends only on `map`, `nA`, and `nB`; no heuristics or
/// look-ahead are used. Complexity is *O(nA)* with *O(1)* extra space.
///
/// \param map One-sided alignment from A indices to B indices (`-1` for
///            unmatched A positions).
/// \param nA  Size of sequence A.
/// \param nB  Size of sequence B.
/// \returns   List of `Hunk` objects, one per contiguous edit region
///            between anchors.
std::vector<Hunk> hunksFromMap(ArrayRef<int64_t> map, size_t nA, size_t nB);

} // namespace diffutils
} // namespace refold
} // namespace clang

// Format providers for both `Hunk` and `Step` structs.
namespace llvm {
template <> struct format_provider<clang::refold::diffutils::Hunk> {
  static void format(const clang::refold::diffutils::Hunk &hunk,
                     raw_ostream &os, StringRef style) {
    std::string hunkStr;
    if (style.equals_insensitive("v") || style.equals_insensitive("verbose")) {
      // Eat the "style" and call the appropriate "to string" function.
      hunkStr.assign(hunk.ToString</*kVerbose*/ true>());
      style = "";
    } else {
      hunkStr.assign(hunk.ToString</*kVerbose*/ false>());
    }
    format_provider<StringRef>::format(hunkStr, os, style);
  }
};
} // namespace llvm

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_DIFFALGORITHMS_H

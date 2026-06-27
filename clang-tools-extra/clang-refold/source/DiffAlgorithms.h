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
//     (typically token spellings), including owner-aware and
//     provenance-certified variants used by the refolder.
//   • Hunk construction from an A→B alignment (contiguous edit regions).
//   • Myers O((N+M)*D) shortest edit script (SES) with linear-space
//     reconstruction.
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
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace diffutils {

// Default to ~4GB budget.
constexpr unsigned long long DEFAULT_MAX_CELLS = 1ULL << 28;

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

  bool operator==(const Hunk &other) const {
    return aStart == other.aStart && aEnd == other.aEnd &&
           bStart == other.bStart && bEnd == other.bEnd;
  }

  bool operator!=(const Hunk &other) const { return !(*this == other); }

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
/// using Myers’ linear-space divide-and-conquer algorithm, returning the edit
/// steps in forward order.
///
/// The implementation finds a middle snake for each active box
/// `A[aLo,aHi) × B[bLo,bHi)`, recursively solves the left and right
/// subproblems, and emits the diagonal snake in between. This preserves the
/// same SES objective as the classic Myers frontier walk, but avoids storing a
/// full trace of every frontier layer.
///
/// **Complexity:**
/// * Time: *O((N+M)·D)*
/// * Space: *O(N+M)*
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


/// \brief Structured provenance for one A-side token gap used by LCS.
///
/// `ownerDepthGap` intentionally collapses nested ownership into one scalar.
/// The scalar remains the primary cost for the core LCS objective, while the
/// identity-bearing fields below are used to certify boundary-preserving
/// ambiguous-edge restoration without looking at neighboring token spellings.
struct LcsAGapProvenance {
  static constexpr uint64_t NoId = std::numeric_limits<uint64_t>::max();

  uint32_t ownerDepth = 0;
  uint32_t includeDepth = 0;
  uint32_t conditionalDepth = 0;
  uint32_t macroDepth = 0;

  uint64_t leftIncludeId = NoId;
  uint64_t rightIncludeId = NoId;
  uint64_t lcaIncludeId = NoId;

  uint64_t leftCondGroupId = NoId;
  uint64_t leftCondArmId = NoId;
  uint64_t rightCondGroupId = NoId;
  uint64_t rightCondArmId = NoId;

  uint64_t leftMacroRootId = NoId;
  uint64_t leftMacroLeafId = NoId;
  uint64_t rightMacroRootId = NoId;
  uint64_t rightMacroLeafId = NoId;

  uint32_t leftMacroRoleMask = 0;
  uint32_t rightMacroRoleMask = 0;
};


/// \brief Edited-side structural surface for one B-side token gap.
///
/// `LcsAGapProvenance` describes where an A-side gap came from in the original
/// preprocessor provenance graph. B-side gap provenance describes the edited
/// insertion island's structural surface: line affinity and whitespace/newline
/// shape around adjacent tokens. It deliberately does not include neighboring
/// token spellings, so using it is not the removed neighbor-coherence heuristic.
struct LcsBGapProvenance {
  static constexpr uint64_t NoOffset = std::numeric_limits<uint64_t>::max();

  bool hasLeftToken = false;
  bool hasRightToken = false;
  bool gapContainsNewline = false;
  bool gapContainsOnlyWs = true;
  bool gapAtLineStart = false;
  bool gapAtLineEnd = false;
  bool leftTokenStartsLine = false;
  bool leftTokenEndsLine = false;
  bool rightTokenStartsLine = false;
  bool rightTokenEndsLine = false;

  // Byte coordinates in the edited preprocessed stream. The production ranking
  // uses the boolean line/whitespace shape above; absolute offsets are retained
  // for trace output and postmortem diagnostics, not as proof inputs.
  uint64_t gapBeginByte = NoOffset;
  uint64_t gapEndByte = NoOffset;
  uint64_t leftTokenBeginByte = NoOffset;
  uint64_t leftTokenEndByte = NoOffset;
  uint64_t rightTokenBeginByte = NoOffset;
  uint64_t rightTokenEndByte = NoOffset;
};

/// \brief Compute an owner-aware LCS backmap from sequence A to B.
///
/// Computes a one-sided longest common subsequence (LCS) mapping from sequence
/// `A` to `B` using the scalar owner-depth cost supplied by the caller. This is
/// the core weighted LCS objective: maximize matched-token count, then minimize
/// the accumulated cost of edits crossing owner gaps.
///
/// Returns an array `map` of length `A.size()` where `map[i] = j` if `A[i]`
/// participates in a valid alignment with `B[j]`, or `-1` if `A[i]` is
/// unmatched.
///
/// ### Structural Constraints (Owner-Awareness)
///
/// The `ownerDepthGap` parameter is indexed by A-side token gaps. Higher values
/// make it more expensive for edits to cross deeper include/conditional owner
/// boundaries. This scalar overload is deterministic but does not perform the
/// full repeated-token ambiguity certification provided by the structured
/// provenance overloads below.
///
/// ### Performance & Scaling
///
/// - **DP Path:** Used for small-to-medium sequences where the product of
///   lengths is less than \p maxCells. Employs a cost-model-augmented
///   Dynamic Programming approach.
/// - **Hirschberg Fallback:** If \p maxCells is exceeded, the algorithm switches
///   to Hirschberg recursion (exact) using O(N+M) space.
///
/// ### Complexity
///
/// * **Time:** O(N*M) (DP or Hirschberg; both exact).
/// * **Space:** O(N*M) for the full DP table, O(N+M) for Hirschberg, O(N) for
///   the map.
///
/// \param a The original (Source A) sequence of tokens/strings.
/// \param b The edited (Source B) sequence of tokens/strings.
/// \param ownerDepthGap A gap array of size `a.size() + 1` giving the
///        ownership/boundary cost at each A-side token gap.
/// \param maxCells The threshold for the DP table size (N*M) before switching
///        to Hirschberg recursion.
/// \returns A vector mapping each index in \p a to its corresponding index
///          in \p b, or -1 if the token was deleted or moved.
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<uint32_t> ownerDepthGap,
                              unsigned long long maxCells = DEFAULT_MAX_CELLS);

/// \brief Compute the provenance-certified owner-aware LCS map.
///
/// This overload derives the scalar owner-depth array from `gapProvenance` and
/// then builds a certified partial map: only core-LCS-forced anchors are kept,
/// and ambiguous equal-token edge anchors are restored only when the structured
/// boundary profile proves a unique pure-insertion frontier.
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<LcsAGapProvenance> gapProvenance,
                              unsigned long long maxCells = DEFAULT_MAX_CELLS);

/// \brief Same as the structured-provenance overload, with edited-side B-gap
/// surface profiles used as the final structural discriminator for otherwise
/// equivalent pure-insertion frontiers.
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<LcsAGapProvenance> gapProvenance,
                              ArrayRef<LcsBGapProvenance> bGapProvenance,
                              unsigned long long maxCells = DEFAULT_MAX_CELLS);

/// \brief Compute a plain deterministic one-sided LCS backmap from A to B.
///
/// This overload is for generic sequence alignment when no owner/provenance
/// information is available. It returns an order-preserving map of length
/// `A.size()` where `map[i] = j` if `A[i]` participates in the selected LCS
/// alignment with `B[j]`, or `-1` if `A[i]` is unmatched.
///
/// This path is deterministic but intentionally does not provide the refolder's
/// owner-aware or provenance-certified ambiguity handling.
///
/// ### Large-input guard
///
/// If the full DP table would exceed the configured cell budget,
/// the algorithm switches to Hirschberg recursion (exact) using O(N+M) space.
///
/// ### Complexity
///
/// * DP path: time *O(N×M)*, space *O(N×M)*
/// * Hirschberg fallback: time *O(N×M)*, space *O(N+M)*
///
/// \param a Left sequence.
/// \param b Right sequence.
/// \param maxCells Maximum number of DP cells before switching to Hirschberg
/// recursion. \returns A vector mapping a-indices to b-indices (or -1 if
/// unmatched).
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

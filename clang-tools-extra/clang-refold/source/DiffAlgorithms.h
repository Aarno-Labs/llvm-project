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
//     the owner-aware LCS objective and exposes only every-optimal-path
//     forced anchors until a separate semantic resolver proves equivalence.
//   • hunksFromMap: convert an A→B map into ordered edit hunks between anchors.
//   • diff / coalesce: produce SES steps (EQUAL/INSERT/DELETE) and merge
//     adjacent non-EQUAL runs into hunks.
//
// Determinism & Policy
// --------------------
//   • Algorithms are deterministic. The refolder-facing LCS overload avoids
//     lexical neighbor tie heuristics by keeping only certified anchors.
//   • Large-input guard: LCS switches to Hirschberg recursion (exact) when
//     the checked aggregate allocation would exceed a configured byte budget.
//   • Utilities are side-effect free and operate on caller-owned sequences.
//
// Complexity
// ----------
//   • LCS:      time O(N*M); DP uses O(N*M) space, Hirschberg uses O(N+M).
//   • diff:     expected time O((N+M)*D), space O(N+M).
//   • Hunking:  O(N) over the alignment/map.
//
// Public Surface
// --------------
//   • std::vector<int64_t> lcsMapAB(...):
//       A[i] -> B[j] (j >= 0) or -1; owner-aware/provenance-certified when
//       structured gap profiles are supplied.
//   • std::vector<Hunk> hunksFromMap(ArrayRef<int64_t> map,
//                                    ArrayRef<LcsCertifiedBoundary> seams,
//                                    size_t nA, size_t nB):
//       contiguous edit regions between anchors, split at exact DP seams.
//   • std::vector<Step> diff(ArrayRef<StringRef> A, ArrayRef<StringRef> B):
//       shortest edit script (EQUAL/INSERT/DELETE).
//   • std::vector<Hunk> coalesce(ArrayRef<Step> steps):
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

// Brings in the generic format providers (optional/ToString/enum/cl::opt) so
// they are visible before this header's own `formatv` uses and before the
// explicit `format_provider<Hunk>` specialization defined at the bottom.
#include "core/RefoldFormatProviders.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace diffutils {

/// Aggregate heap-payload budget for one quadratic LCS proof/realization.
///
/// The previous limit counted DP cells and therefore understated the real
/// allocation by several simultaneously live tables. One GiB is intentionally
/// below the multi-gigabyte range while remaining large enough for ordinary
/// translation-unit alignments.
constexpr unsigned long long DEFAULT_MAX_BYTES = 1ULL << 30;

/// Aggregate retained payload budget for optional admissible-pair diagnostics.
///
/// The production proof tables are governed separately by `DEFAULT_MAX_BYTES`.
/// Trace mode may expose a quadratic number of non-forced optimal match pairs,
/// so their owning vectors receive an independent bounded ledger. Sixty-four
/// MiB retains substantial diagnostic evidence without allowing observability
/// to add an unbounded quadratic payload after certification has succeeded.
constexpr unsigned long long DEFAULT_MAX_DIAGNOSTIC_EVIDENCE_BYTES =
    64ULL << 20;

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

/// Exact value of the core weighted-LCS objective.
///
/// The objective has only two components, evaluated lexicographically:
/// maximize `matchedTokenCount`, then minimize `ownerDepthCost`. Structural
/// boundary profiles may certify or suppress individual anchors after this
/// objective is solved, but they never alter the objective itself.
struct LcsObjective {
  uint32_t matchedTokenCount = 0;
  uint64_t ownerDepthCost = 0;

  bool operator==(const LcsObjective &other) const {
    return matchedTokenCount == other.matchedTokenCount &&
           ownerDepthCost == other.ownerDepthCost;
  }

  bool operator!=(const LcsObjective &other) const {
    return !(*this == other);
  }
};

/// Theorem that authorizes one A-to-B token anchor in the production map.
enum class LcsAnchorProofKind : uint8_t {
  /// No theorem authorizes an anchor at this A-token position.
  None,
  /// Every core-optimal weighted-LCS path through the anchor's certified
  /// window uses this exact match edge.
  CoreOptimalPathForced,
  /// A semantic alignment resolver proved that every remaining admissible
  /// explanation produces one equivalent normalized owner/edit realization.
  EquivalentNormalizedHunkAndOwner,
};

inline StringRef toString(LcsAnchorProofKind kind) {
  switch (kind) {
  case LcsAnchorProofKind::None:
    return "None";
  case LcsAnchorProofKind::CoreOptimalPathForced:
    return "CoreOptimalPathForced";
  case LcsAnchorProofKind::EquivalentNormalizedHunkAndOwner:
    return "EquivalentNormalizedHunkAndOwner";
  }
  llvm_unreachable("invalid LCS anchor proof kind");
}

/// Durable theorem reference for one selected production anchor.
struct LcsAnchorProof {
  LcsAnchorProofKind kind = LcsAnchorProofKind::None;
  /// Nonzero only for `EquivalentNormalizedHunkAndOwner`; identifies the
  /// semantic resolver witness that discharged the ambiguity.
  uint64_t semanticWitnessId = 0;

  bool IsAuthorized() const {
    if (kind == LcsAnchorProofKind::CoreOptimalPathForced)
      return semanticWitnessId == 0;
    if (kind == LcsAnchorProofKind::EquivalentNormalizedHunkAndOwner)
      return semanticWitnessId != 0;
    return false;
  }
};

/// Theorem that authorizes one exact A/B dynamic-programming boundary.
enum class LcsBoundaryProofKind : uint8_t {
  /// No theorem authorizes this boundary.
  None,
  /// Every core-optimal weighted-LCS path through the enclosing window
  /// crosses this exact DP state.
  EveryOptimalPathCrossesState,
  /// A producer-backed theorem proved that every remaining admissible
  /// frontier induces the same downstream normalized result.
  EquivalentDownstreamResult,
};

inline StringRef toString(LcsBoundaryProofKind kind) {
  switch (kind) {
  case LcsBoundaryProofKind::None:
    return "None";
  case LcsBoundaryProofKind::EveryOptimalPathCrossesState:
    return "EveryOptimalPathCrossesState";
  case LcsBoundaryProofKind::EquivalentDownstreamResult:
    return "EquivalentDownstreamResult";
  }
  llvm_unreachable("invalid LCS boundary proof kind");
}

/// Certification disposition for one half-open A/B alignment rectangle.
enum class LcsWindowCertificationStatus : uint8_t {
  /// Complete all-optimal state was materialized for this window.
  Certified,
  /// The checked local allocation exceeded the configured proof budget.
  BudgetExceeded,
  /// The window could not be certified for a non-budget proof reason.
  PartitionUnresolved,
};

inline StringRef toString(LcsWindowCertificationStatus status) {
  switch (status) {
  case LcsWindowCertificationStatus::Certified:
    return "Certified";
  case LcsWindowCertificationStatus::BudgetExceeded:
    return "BudgetExceeded";
  case LcsWindowCertificationStatus::PartitionUnresolved:
    return "PartitionUnresolved";
  }
  llvm_unreachable("invalid LCS window certification status");
}

/// Exact certified seam between two independently alignable A/B regions.
struct LcsCertifiedBoundary {
  uint64_t aBoundary = 0;
  uint64_t bBoundary = 0;
  LcsBoundaryProofKind proofKind = LcsBoundaryProofKind::None;

  bool IsAuthorized() const {
    return proofKind != LcsBoundaryProofKind::None;
  }
};

/// Certification record for one half-open A/B token window.
struct LcsCertificationWindow {
  uint64_t aBegin = 0;
  uint64_t aEnd = 0;
  uint64_t bBegin = 0;
  uint64_t bEnd = 0;

  LcsWindowCertificationStatus status =
      LcsWindowCertificationStatus::PartitionUnresolved;
  /// Checked peak heap payload required by the local all-optimal certifier.
  uint64_t requiredBytes = 0;
  /// Proof byte budget applied to this local certification attempt.
  uint64_t proofBudgetBytes = 0;

  bool IsCertified() const {
    return status == LcsWindowCertificationStatus::Certified;
  }

  /// Return true when this certified rectangle contains the complete match
  /// edge from `(aToken,bToken)` to `(aToken+1,bToken+1)`.
  bool ContainsAnchor(uint64_t aToken, uint64_t bToken) const {
    return IsCertified() && aBegin <= aToken && aToken < aEnd &&
           bBegin <= bToken && bToken < bEnd;
  }
};

/// Kind of endpoint delimiting one diagnostic ambiguity window.
///
/// A partition seam is an exact DP boundary rather than a matched token.
/// Keeping it distinct from stream sentinels and forced edges lets local
/// certification publish precise evidence without encoding boundaries as
/// synthetic or negative token indices.
enum class LcsDiagnosticWindowAnchorKind : uint8_t {
  StreamBegin,
  CertifiedBoundary,
  ForcedToken,
  StreamEnd,
};

/// One forced token, exact partition seam, or explicit stream endpoint.
///
/// For `ForcedToken`, the coordinates are zero-based token indices. For every
/// other kind they are DP boundary coordinates. Stream begin is `(0,0)` and
/// stream end is `(globalATokenCount,globalBTokenCount)`.
struct LcsDiagnosticWindowAnchor {
  LcsDiagnosticWindowAnchorKind kind =
      LcsDiagnosticWindowAnchorKind::StreamBegin;
  uint64_t aToken = 0;
  uint64_t bToken = 0;

  static constexpr LcsDiagnosticWindowAnchor StreamBegin() { return {}; }

  static constexpr LcsDiagnosticWindowAnchor
  CertifiedBoundary(uint64_t aBoundary, uint64_t bBoundary) {
    return {LcsDiagnosticWindowAnchorKind::CertifiedBoundary, aBoundary,
            bBoundary};
  }

  static constexpr LcsDiagnosticWindowAnchor ForcedToken(uint64_t aToken,
                                                          uint64_t bToken) {
    return {LcsDiagnosticWindowAnchorKind::ForcedToken, aToken, bToken};
  }

  static constexpr LcsDiagnosticWindowAnchor
  StreamEnd(uint64_t aTokenCount, uint64_t bTokenCount) {
    return {LcsDiagnosticWindowAnchorKind::StreamEnd, aTokenCount,
            bTokenCount};
  }
};

/// Evidence-only request for one physical protected-boundary identity.
///
/// The higher-level source model owns the identity metadata. The LCS layer
/// needs only its stable diagnostic id and exact A projection. An absent
/// boundary preserves an individually unmappable obligation without allowing
/// it to participate in proof scheduling.
struct LcsProtectedBoundaryDiagnosticIdentity {
  uint64_t identityId = 0;
  std::optional<uint64_t> aBoundary;
};

/// One non-forced match edge admitted by an optimal conditioned alignment.
struct LcsDiagnosticAdmissiblePair {
  uint64_t aToken = 0;
  uint64_t bToken = 0;
  bool repeatedInA = false;
  bool repeatedInB = false;
};

/// Exact B-frontier evidence for one protected-boundary identity.
struct LcsProtectedBoundaryDiagnosticProjection {
  uint64_t boundaryIdentityId = 0;
  std::optional<uint64_t> aBoundary;
  std::vector<uint64_t> admissibleBFrontiers;
  bool projectionComplete = false;
};

/// Owning evidence for one forced-anchor-delimited ambiguity window.
///
/// These records are materialized while the local quadratic theorem is live.
/// They are diagnostics only: no field can authorize an anchor, choose a seam,
/// or affect candidate order. An uncertified local rectangle produces one
/// status-bearing record with no pair or B-frontier facts.
struct LcsAmbiguityWindowDiagnosticRecord {
  uint64_t aBegin = 0;
  uint64_t aEnd = 0;
  uint64_t bBegin = 0;
  uint64_t bEnd = 0;

  LcsDiagnosticWindowAnchor leftForcedAnchor =
      LcsDiagnosticWindowAnchor::StreamBegin();
  LcsDiagnosticWindowAnchor rightForcedAnchor =
      LcsDiagnosticWindowAnchor::StreamEnd(0, 0);

  LcsObjective objective;
  std::vector<LcsDiagnosticAdmissiblePair> admissiblePairs;
  std::vector<LcsProtectedBoundaryDiagnosticProjection>
      protectedBoundaryProjections;

  LcsWindowCertificationStatus certificationStatus =
      LcsWindowCertificationStatus::PartitionUnresolved;
  /// True only when the production all-optimal theorem certified this window.
  ///
  /// The two evidence flags below are deliberately independent. Diagnostic
  /// retention or projection may be incomplete without weakening this proof
  /// fact or any anchor authorized by it.
  bool coreCertificationComplete = false;
  /// True only when `admissiblePairs` is the complete non-forced pair set.
  bool admissiblePairEnumerationComplete = false;
  /// True only when every retained protected identity has an exact projection.
  bool boundaryProjectionComplete = false;
};

/// Complete set of distinct match maps for one conditioned optimal window.
///
/// Distinct DP paths that differ only in insertion/deletion interleaving but
/// carry the same ordered match edges collapse to one map. `complete` is false
/// when the exact set exceeds the caller's proof budget; in that case `maps` is
/// empty and no partial enumeration may be used as authority.
struct OptimalLcsMapEnumeration {
  std::vector<std::vector<int64_t>> maps;
  bool complete = false;
};

/// Immutable all-optimal state for one certified weighted-LCS problem.
///
/// Its internal storage retains the exact core DP facts and admissible-pair
/// relation needed by pair, window-objective, and boundary-frontier queries.
/// Callers cannot mutate those facts or manufacture a completed
/// certification.
///
/// Every coordinate in this class is *oracle-local*: index zero denotes the
/// first token of the certified rectangle this oracle was built from, not the
/// first token of the complete stream. The historical complete-stream oracle
/// satisfies both readings because its rectangle begins at `(0,0)`; an oracle
/// retained for an interior certification window does not, so its callers must
/// translate by that window's `aBegin`/`bBegin` before issuing a query and
/// translate returned B indices back afterwards. `CertifiedLcsResult` exposes
/// `certificationWindows[i]` alongside `windowOracles[i]` precisely so that
/// translation reads its origin from a recorded fact.
class OptimalTokenAlignmentOracle {
public:
  struct Storage;

  OptimalTokenAlignmentOracle() = default;
  explicit OptimalTokenAlignmentOracle(std::shared_ptr<const Storage> storage)
      : storage_(std::move(storage)) {}

  /// Return true when the complete all-optimal state was materialized.
  bool HasCompleteCertification() const;
  /// Number of A-side tokens represented by this oracle.
  size_t GetATokenCount() const;
  /// Number of B-side tokens represented by this oracle.
  size_t GetBTokenCount() const;

  /// Return true when the equal-token pair occurs on at least one globally
  /// optimal path for the unchanged weighted-LCS objective.
  ///
  /// A false result means either that the pair is not globally admissible or
  /// that this oracle has incomplete certification. Callers that need to
  /// distinguish those cases must first check `HasCompleteCertification()`.
  bool PairOccursOnOptimalPath(uint64_t aToken, uint64_t bToken) const;

  /// Compute the exact weighted-LCS objective inside one half-open A/B window.
  ///
  /// The query uses the same absolute A-gap costs as the global problem. It is
  /// a local optimum and does not by itself assert that the window endpoints
  /// occur together on a globally optimal path. Invalid bounds or incomplete
  /// certification return the default objective; callers should validate the
  /// oracle and bounds before issuing the query.
  LcsObjective ObjectiveForWindow(uint64_t aBegin, uint64_t aEnd,
                                  uint64_t bBegin, uint64_t bEnd) const;

  /// Enumerate every distinct core-optimal match map inside one conditioned
  /// forced-anchor window.
  ///
  /// The enumeration is exact and deduplicates insertion/deletion interleavings
  /// that induce the same ordered set of match edges. If the number of distinct
  /// maps exceeds `maxUniqueMaps`, the result is incomplete and contains no
  /// maps; callers must retain only already-forced anchors.
  OptimalLcsMapEnumeration EnumerateOptimalMapsForWindow(
      uint64_t aWindowBegin, uint64_t aWindowEnd, uint64_t bWindowBegin,
      uint64_t bWindowEnd, size_t maxUniqueMaps) const;

private:
  std::shared_ptr<const Storage> storage_;
};

/// Exact result of certifying one conditioned A/B alignment window.
///
/// The vectors are indexed relative to `window.aBegin`, but every nonnegative
/// B value is an absolute index in the caller's complete B token stream. This
/// representation keeps the result proportional to the local A width while
/// making independently certified windows directly mergeable into one global
/// map without another coordinate-conversion theorem.
///
/// The core window certifier initializes `selectedMap` from `forcedMap`; it
/// does not perform semantic restoration. Quadratic all-optimal state is
/// deliberately not retained here, so it is released when
/// `certifyLcsWindow()` returns. The complete-stream compatibility path may
/// retain that state separately in its `OptimalTokenAlignmentOracle` to
/// preserve existing behavior.
struct LcsWindowCertificationResult {
  LcsCertificationWindow window;
  LcsObjective objective;
  bool objectiveIsExact = false;
  std::vector<int64_t> forcedMap;
  std::vector<int64_t> selectedMap;
  std::vector<LcsAnchorProof> selectedAnchorProofs;

  bool IsCertified() const { return window.IsCertified(); }
};

/// Exact linear-space projection of one A boundary into an enclosing B range.
///
/// `admissibleBFrontiers` contains every B-side DP frontier at which the
/// enclosing window's complete weighted objective decomposes exactly into its
/// left and right subobjectives. The vector is sorted and absolute. A singleton
/// vector therefore proves that every optimal path crosses one exact DP state;
/// no selected Hirschberg split or boundary-ranking policy contributes to the
/// result.
struct LcsBoundaryFrontierProjection {
  uint64_t aBoundary = 0;
  LcsObjective windowObjective;
  bool objectiveIsExact = false;
  std::vector<uint64_t> admissibleBFrontiers;

  bool ProvesUniqueBoundary() const {
    return objectiveIsExact && admissibleBFrontiers.size() == 1;
  }
};

/// Exact state-seam partition produced without a quadratic DP table.
///
/// Every boundary carries an `EveryOptimalPathCrossesState` theorem. The
/// resulting windows cover the requested rectangle in source order and remain
/// `PartitionUnresolved` until the local quadratic certifier processes them.
/// This separation keeps seam proof linear-space and lets callers decide when
/// to spend quadratic proof memory on each independent window.
struct LcsWindowPartitionResult {
  LcsObjective objective;
  bool objectiveIsExact = false;
  llvm::SmallVector<LcsCertifiedBoundary, 8> certifiedBoundaries;
  llvm::SmallVector<LcsCertificationWindow, 8> windows;
};

/// Optional evidence ledger for permanent certification diagnostics.
///
/// This record is populated only when a caller explicitly supplies it. Keeping
/// frontier vectors outside `CertifiedLcsResult` prevents diagnostic retention
/// from increasing ordinary production memory or changing the proof result.
struct LcsCertificationDiagnosticEvidence {
  /// Maximum aggregate payload retained by all admissible-pair vectors.
  ///
  /// This is an evidence-only input. Certification routines preserve the
  /// configured limit while resetting the generated byte charge for each run.
  uint64_t admissiblePairByteBudget =
      DEFAULT_MAX_DIAGNOSTIC_EVIDENCE_BYTES;
  /// Checked aggregate payload currently retained in `ambiguityWindows`.
  uint64_t retainedAdmissiblePairBytes = 0;
  /// Canonically ordered physical identities requested by the caller.
  ///
  /// This is the only input portion of the ledger. Certification routines
  /// clear and repopulate every output field while preserving these requests.
  /// They are never consulted for partition scheduling or anchor authority.
  llvm::SmallVector<LcsProtectedBoundaryDiagnosticIdentity, 8>
      protectedBoundaryIdentities;
  /// Sorted unique A boundaries nominated for the exact frontier theorem.
  llvm::SmallVector<uint64_t, 8> candidateABoundaries;
  /// Exact admissible B-frontier sets for every boundary actually tested.
  llvm::SmallVector<LcsBoundaryFrontierProjection, 8>
      boundaryFrontierProjections;
  /// Immutable ambiguity evidence captured before each local DP is released.
  ///
  /// Complete-stream and partitioned runs populate this same canonical ledger.
  /// A certified local rectangle contributes its actual forced-anchor-delimited
  /// ambiguity records; an uncertified rectangle contributes exactly one
  /// status-only record matching that rectangle. A partitioned run therefore
  /// never collapses to a synthetic full-stream incomplete record.
  std::vector<LcsAmbiguityWindowDiagnosticRecord> ambiguityWindows;
};

/// Structured result of the provenance-certified weighted LCS.
///
/// `forcedMap` contains only anchors forced by the unchanged core objective.
/// The core certifier initializes `selectedMap` from that exact surface. A
/// later semantic alignment resolver may add non-forced anchors only after it
/// records one durable `EquivalentNormalizedHunkAndOwner` witness. Every mapped
/// A token has a parallel `selectedAnchorProofs` entry naming its authority.
///
/// Certification is window-local. A failed window contributes no anchors, but
/// anchors already proved in independent certified windows remain valid. The
/// current implementation initially represents the complete stream as one
/// window; later partitioning patches may populate certified and uncertified
/// windows simultaneously without changing this authority contract.
struct CertifiedLcsResult {
  std::vector<int64_t> forcedMap;
  std::vector<int64_t> selectedMap;
  std::vector<LcsAnchorProof> selectedAnchorProofs;
  OptimalTokenAlignmentOracle oracle;
  /// Exact optimum for the complete A/B token streams when the companion
  /// exactness flag is true, even if one or more windows are uncertified.
  LcsObjective globalObjective;
  bool globalObjectiveIsExact = false;
  /// Aggregate convenience bit; partition validation requires it to equal the
  /// conjunction of every window's `Certified` status.
  bool allWindowsCertified = false;
  /// Keep the initial full-stream window inline so this structural patch does
  /// not change the existing proof-budget threshold. Later partitioning may
  /// grow these ledgers without changing their ordered semantics.
  llvm::SmallVector<LcsCertifiedBoundary, 1> certifiedBoundaries;
  llvm::SmallVector<LcsCertificationWindow, 1> certificationWindows;
  /// Optional all-optimal oracles retained for individual windows.
  ///
  /// The vector is either empty or exactly parallel to `certificationWindows`.
  /// An entry is populated only by an explicit `retainCertifiedWindowOracle()`
  /// request whose independent recertification reproduced the forced anchors
  /// already published for that window, so a present oracle is a checked fact
  /// rather than a second opinion. Entries are in window-local coordinates; see
  /// `OptimalTokenAlignmentOracle` for the translation contract.
  ///
  /// Retention is deliberately per window and on demand. The quadratic cost of
  /// these facts is what partitioning exists to avoid, so retaining every
  /// window would reintroduce exactly the complete-grid payload the partition
  /// scheduler refused to allocate.
  llvm::SmallVector<OptimalTokenAlignmentOracle, 1> windowOracles;

  /// Return true when an A/B match edge lies wholly inside a certified window.
  bool AnchorBelongsToCertifiedWindow(uint64_t aToken,
                                      uint64_t bToken) const;

  /// Validate the ordered window partition and its aggregate status.
  ///
  /// Empty-sided insertion/deletion windows are valid. A window with no A or B
  /// progress is valid only for the single empty-stream partition.
  bool CertificationPartitionIsWellFormed(uint64_t aTokenCount,
                                          uint64_t bTokenCount) const;

  /// Return true when the retained oracle certifies one complete-stream
  /// window. Low-level consumers of complete global pair facts should use this
  /// query rather than inferring authority from an aggregate Boolean; semantic
  /// restoration should use `HasCompleteSemanticOracleForWindow()` so its
  /// per-window availability remains explicit.
  bool HasCompleteGlobalOracle() const;

  /// Return true when semantic restoration may enumerate every optimal map in
  /// one certification window.
  ///
  /// A `Certified` status proves core-forced anchors, but it does not imply
  /// that the quadratic pair facts needed by semantic equivalence remain
  /// available. Two independent surfaces can supply them: the historical
  /// complete-stream compatibility oracle, which is available only when the
  /// whole stream certified as one window, and a per-window oracle installed by
  /// `retainCertifiedWindowOracle()`. Every other window answers false, so a
  /// caller can never read pair facts from a window whose facts were released.
  ///
  /// Availability is deliberately per window rather than aggregate. Ambiguity
  /// inside a certified window is resolvable on that window's own facts; a
  /// neighboring uncertified window contributes no anchors to any candidate and
  /// therefore cannot make one candidate look better than another.
  bool HasCompleteSemanticOracleForWindow(size_t windowIndex) const;

  /// Return the oracle usable for semantic restoration inside one window.
  ///
  /// Returns null unless `HasCompleteSemanticOracleForWindow(windowIndex)`
  /// holds. The returned oracle is in window-local coordinates; callers must
  /// translate by `certificationWindows[windowIndex]`'s origin.
  const OptimalTokenAlignmentOracle *
  GetSemanticOracleForWindow(size_t windowIndex) const;

  /// Discard every semantic-restored selection while preserving all
  /// independently certified core anchors.
  ///
  /// This operation is the conservative boundary used when semantic
  /// restoration lacks complete explanations for one or more windows. It does
  /// not alter `forcedMap`, certification windows, seams, or the exact global
  /// objective.
  void RetainOnlyCoreForcedAnchors();
};

/// Format one stable evidence-only certification transcript.
///
/// The returned lines describe the complete global objective, every tested
/// partition boundary and admissible frontier set, accepted seams, local
/// certification windows, retained proof kinds, and exact failed rectangles.
/// Formatting reads only the finished result and cannot affect candidate order,
/// map publication, or any proof decision.
std::vector<std::string>
describeLcsCertificationRun(
    const CertifiedLcsResult &result,
    const LcsCertificationDiagnosticEvidence &diagnosticEvidence,
    uint64_t aTokenCount, uint64_t bTokenCount);

/// \brief Structured provenance for one A-side token gap used by LCS.
///
/// `ownerDepthGap` intentionally collapses nested ownership into one scalar.
/// The scalar remains the primary cost for the core LCS objective, while the
/// identity-bearing fields below remain available to downstream owner and
/// structural proofs; they do not select non-forced alignment anchors.
struct LcsAGapProvenance {
  static constexpr uint64_t noId = std::numeric_limits<uint64_t>::max();

  uint32_t ownerDepth = 0;
  uint32_t includeDepth = 0;
  uint32_t conditionalDepth = 0;
  uint32_t macroDepth = 0;

  uint64_t leftIncludeId = noId;
  uint64_t rightIncludeId = noId;
  uint64_t lcaIncludeId = noId;

  uint64_t leftCondGroupId = noId;
  uint64_t leftCondArmId = noId;
  uint64_t rightCondGroupId = noId;
  uint64_t rightCondArmId = noId;

  uint64_t leftMacroRootId = noId;
  uint64_t leftMacroLeafId = noId;
  uint64_t rightMacroRootId = noId;
  uint64_t rightMacroLeafId = noId;

  uint32_t leftMacroRoleMask = 0;
  uint32_t rightMacroRoleMask = 0;
};

/// Certify one exact half-open A/B token window.
///
/// The local dynamic program is conditioned on entering at
/// `(aBegin,bBegin)` and leaving at `(aEnd,bEnd)`. It uses the unchanged
/// weighted-LCS objective and the same all-optimal-path dominator theorem as
/// `certifiedLcsMapAB()`. No selected Hirschberg path is published.
///
/// On success, the returned maps have length `aEnd-aBegin`; mapped B indices
/// are absolute. On a proof-budget failure, the result records
/// `BudgetExceeded`, the checked local byte requirement, and the exact local
/// objective while keeping both maps fully suppressed. Invalid bounds or an
/// unrepresentable local state space produce `PartitionUnresolved`.
///
/// Quadratic tables, pair facts, and dominator state are local to this call and
/// are destroyed before it returns. This permits callers to certify windows
/// sequentially with peak quadratic storage determined by the largest window.
bool certifyLcsWindow(
    ArrayRef<StringRef> a, uint64_t aBegin, uint64_t aEnd,
    ArrayRef<StringRef> b, uint64_t bBegin, uint64_t bEnd,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    unsigned long long maxBytes, LcsWindowCertificationResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence = nullptr);

/// Retain the all-optimal oracle for one already-certified window on demand.
///
/// The window is certified a second time, in isolation, with its quadratic
/// pair facts retained instead of released. The recertification is admitted
/// only when it reproduces the forced anchors already published for that
/// window byte-for-byte; a disagreement leaves the result untouched and returns
/// false, so this routine can add proof surface but never revise a published
/// one. It also never changes `certificationWindows`, seams, maps, anchor
/// proofs, or the global objective.
///
/// Retention is charged the complete-oracle payload, which is strictly larger
/// than the payload the original pass checked, because the oracle keeps its own
/// owner-gap copy alive. A window that certified without retention may
/// therefore be unaffordable with it; that case returns false and is a
/// successful fail-closed outcome, not an error.
///
/// Returns true when `result.windowOracles[windowIndex]` is populated, either
/// by this call or because the window already had a usable oracle.
bool retainCertifiedWindowOracle(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance, unsigned long long maxBytes,
    size_t windowIndex, CertifiedLcsResult &result);

/// Compute the exact checked heap-payload requirement for one certification
/// rectangle without running dynamic programming or allocating its tables.
///
/// `retainCompleteOracle` must be true for the historical complete-stream path
/// and false for an isolated local window. The distinction accounts for the
/// additional retained owner-gap copy in the complete oracle. False means the
/// rectangle or compact dominator index is not representable.
bool getLcsCertificationRequiredBytes(uint64_t aTokenCount,
                                      uint64_t bTokenCount,
                                      bool retainCompleteOracle,
                                      uint64_t &requiredBytes);

/// Project one A boundary to every exact optimal B frontier in linear space.
///
/// The complete weighted objective is used: matched-token count is maximized
/// first and owner-depth cost is minimized second. The routine retains only a
/// constant number of B-width rows and never reconstructs one selected path.
/// Invalid bounds or malformed provenance return false and leave an incomplete
/// result.
bool projectLcsBoundaryToOptimalBFrontiers(
    ArrayRef<StringRef> a, uint64_t aBegin, uint64_t aEnd,
    ArrayRef<StringRef> b, uint64_t bBegin, uint64_t bEnd,
    uint64_t aBoundary, ArrayRef<LcsAGapProvenance> gapProvenance,
    LcsBoundaryFrontierProjection &result);

/// Nominate deterministic, identity-bearing A partition boundaries.
///
/// The returned vector is sorted and unique. It contains exact provenance
/// transitions, including stream endpoints when independently nominated, the
/// endpoints of maximal runs of already-forced token edges,
/// and any caller-supplied producer/protected-structure boundaries. Candidate
/// nomination grants no B-side authority; `partitionLcsWindowsLinearSpaceImpl()`
/// must still prove a singleton optimal frontier.
std::vector<uint64_t> nominateLcsPartitionBoundaries(
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<int64_t> forcedMap = {},
    ArrayRef<uint64_t> additionalABoundaries = {});

/// Partition the complete streams and certify each resulting window locally.
///
/// Nonempty candidate A boundaries are first proved with
/// `partitionLcsWindowsLinearSpaceImpl()`. Every resulting window is then passed,
/// in source order, to the existing bounded all-optimal certifier. Certified
/// anchors are translated into the complete-stream maps; an uncertified window
/// contributes no anchors but does not revoke anchors or exact seams proved in
/// independent windows.
///
/// `maxBytes` is a per-window proof budget. Each window records its own checked
/// `requiredBytes`, applied `proofBudgetBytes`, and final status. Quadratic
/// state is local to one `certifyLcsWindow()` call and is destroyed before the
/// next window is processed, so peak quadratic storage is determined by the
/// largest individual window rather than the complete A/B grid. The optional
/// diagnostic ledger is run-scoped and receives every local record in source
/// order, including one status-only record for each failed rectangle. When no
/// interior seam is proved, the established one-window compatibility path is
/// reused through that same collector, including its retained complete-stream
/// oracle and historical byte threshold.
///
/// The function returns false only when the inputs or composed proof result are
/// malformed. Local `BudgetExceeded` and `PartitionUnresolved` statuses are
/// successful fail-closed outcomes represented in `result`; callers must
/// inspect `allWindowsCertified` rather than interpreting the return value as
/// aggregate certification.
///
/// Callers that supply nonempty nominations must pass the resulting
/// `certifiedBoundaries` to seam-aware hunk construction so independently
/// certified regions cannot be recombined. Supplying a diagnostic-evidence
/// pointer records frontier sets but does not alter proof or map results.
bool certifyLcsWindowsIndependently(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<uint64_t> candidateABoundaries,
    unsigned long long maxBytes, CertifiedLcsResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence = nullptr);

/// Partition only oversized windows, using exact balanced state-seam proofs.
///
/// This is the production scalability path. A complete-stream theorem that
/// exceeds `maxBytes` is divided recursively. Each oversized window tests its
/// arithmetic A midpoint first, then at most one nearest producer-nominated A
/// boundary from the middle half of the window if the midpoint has multiple
/// admissible B frontiers. A split is accepted only when the complete weighted
/// objective proves one unique B frontier; A-boundary scheduling never selects
/// B-side authority.
///
/// An accepted midpoint halves the A width; an accepted producer fallback
/// leaves each child with at most three quarters of the parent A width. The sum
/// of child-grid areas therefore decreases geometrically, independent of the
/// proved B frontier. Consequently recursive partitioning performs only a
/// bounded constant multiple of one complete-grid linear-space pass instead of
/// one complete pass per structural candidate.
/// Windows that already fit the local proof budget are not projected again.
/// An oversized window for which neither tested A boundary proves a unique
/// frontier remains one fail-closed window.
///
/// Producer nominations are normalized and used only as deterministic fallback
/// A coordinates. They never nominate or rank a B frontier. Complete-stream,
/// partitioned, mixed, and wholly unresolved outcomes publish through one
/// run-scoped diagnostic collector. Evidence records every boundary actually
/// tested and every local ambiguity/failure record, but cannot affect proof
/// order or results.
bool certifyLcsWindowsWithinBudget(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<uint64_t> candidateABoundaries,
    unsigned long long maxBytes, CertifiedLcsResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence = nullptr);

/// \brief Edited-side structural surface for one B-side token gap.
///
/// `LcsAGapProvenance` describes where an A-side gap came from in the original
/// preprocessor provenance graph. B-side gap provenance describes the edited
/// insertion island's structural surface: line affinity and whitespace/newline
/// shape around adjacent tokens. It deliberately does not include neighboring
/// token spellings, so using it is not the removed neighbor-coherence
/// heuristic.
struct LcsBGapProvenance {
  static constexpr uint64_t noOffset = std::numeric_limits<uint64_t>::max();

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

  // Byte coordinates in the edited preprocessed stream. All fields are retained
  // only for compatibility and trace output; none is an anchor-selection proof.
  uint64_t gapBeginByte = noOffset;
  uint64_t gapEndByte = noOffset;
  uint64_t leftTokenBeginByte = noOffset;
  uint64_t leftTokenEndByte = noOffset;
  uint64_t rightTokenBeginByte = noOffset;
  uint64_t rightTokenEndByte = noOffset;
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
/// - **DP Path:** Used for small-to-medium sequences when the checked aggregate
///   allocation fits within \p maxBytes. Employs a cost-model-augmented
///   Dynamic Programming approach.
/// - **Hirschberg Fallback:** If \p maxBytes is exceeded, the algorithm switches
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
/// \param maxBytes Maximum aggregate heap payload admitted for the quadratic
///        path before switching to Hirschberg recursion.
/// \returns A vector mapping each index in \p a to its corresponding index
///          in \p b, or -1 if the token was deleted or moved.
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<uint32_t> ownerDepthGap,
                              unsigned long long maxBytes = DEFAULT_MAX_BYTES);

/// Build the structured provenance-certified weighted-LCS result.
///
/// The returned oracle and `globalObjective` describe only the unchanged core
/// objective: maximize matched-token count, then minimize owner-depth cost.
/// `selectedMap` initially equals the exact every-optimal-path `forcedMap`.
/// Non-forced production anchors belong to the separate semantic alignment
/// resolver and therefore cannot contaminate the all-optimal oracle. If the
/// all-optimal tables exceed the proof budget, the single full-stream window
/// is marked `BudgetExceeded` and both maps remain fully suppressed.
CertifiedLcsResult
certifiedLcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                  ArrayRef<LcsAGapProvenance> gapProvenance,
                  unsigned long long maxBytes = DEFAULT_MAX_BYTES,
                  LcsCertificationDiagnosticEvidence *diagnosticEvidence =
                      nullptr);

/// \brief Compute the provenance-certified owner-aware LCS map.
///
/// This overload derives the scalar owner-depth array from `gapProvenance` and
/// then builds a certified partial map containing only core-LCS-forced
/// anchors. Ambiguous equal-token anchors remain suppressed until the separate
/// semantic resolver proves that every optimal explanation induces one
/// equivalent normalized owner/edit realization. A failed certification
/// window contributes no anchors; in the current one-window implementation,
/// that conservatively suppresses the complete returned map rather than
/// selecting one uncertified weighted LCS.
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<LcsAGapProvenance> gapProvenance,
                              unsigned long long maxBytes = DEFAULT_MAX_BYTES);

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
/// If the checked quadratic allocation would exceed the configured byte budget,
/// the algorithm switches to Hirschberg recursion (exact) using O(N+M) space.
///
/// ### Complexity
///
/// * DP path: time *O(N×M)*, space *O(N×M)*
/// * Hirschberg fallback: time *O(N×M)*, space *O(N+M)*
///
/// \param a Left sequence.
/// \param b Right sequence.
/// \param maxBytes Maximum aggregate heap payload for the quadratic path.
/// \returns A vector mapping a-indices to b-indices (or -1 if unmatched).
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              unsigned long long maxBytes = DEFAULT_MAX_BYTES);

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
/// The function walks each A/B rectangle delimited by `certifiedBoundaries`
/// independently. Within one rectangle it emits a `Hunk` for every region
/// between consecutive token anchors, including the leading and trailing
/// regions. A certified boundary is therefore a hard edit-region cut even
/// when no matched token crosses that DP state. The seam itself is never
/// represented as an equal token or synthetic anchor.
///
/// Each hunk describes the half-open intervals `A[aStart,aEnd)` and
/// `B[bStart,bEnd)` that differ:
/// * **INSERT:** `aStart == aEnd && bStart < bEnd`
/// * **DELETE:** `aStart < aEnd && bStart == bEnd`
/// * **REPLACE:** `aStart < aEnd && bStart < bEnd`
///
/// Authorized seams must be ordered, monotone, interior DP states within the
/// complete A/B rectangle. Every mapped edge must remain inside exactly one
/// seam-delimited rectangle. Malformed proof input is rejected rather than
/// being interpreted as an alignment hint.
///
/// **Determinism:**
/// Output depends only on `map`, the exact certified seam sequence, `nA`, and
/// `nB`; no heuristic or selected-path information is used. Complexity is
/// *O(nA + certifiedBoundaries.size())* with *O(1)* auxiliary state beyond
/// the returned hunk vector.
///
/// \param map One-sided alignment from A indices to B indices (`-1` for
///            unmatched A positions).
/// \param certifiedBoundaries Exact DP-state seams that must split hunks.
/// \param nA  Size of sequence A.
/// \param nB  Size of sequence B.
/// \returns   List of `Hunk` objects, one per contiguous edit region inside
///            one seam-delimited rectangle.
std::vector<Hunk>
hunksFromMap(ArrayRef<int64_t> map,
             ArrayRef<LcsCertifiedBoundary> certifiedBoundaries, size_t nA,
             size_t nB);

/// Backward-compatible hunk construction for callers with no certified seams.
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

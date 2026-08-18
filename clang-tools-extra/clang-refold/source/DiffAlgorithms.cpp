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

#include "source/DiffAlgorithms.h"

#include "core/RefoldLog.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace clang {
namespace refold {
namespace diffutils {

constexpr size_t MAX = static_cast<size_t>(std::numeric_limits<int64_t>::max());

/// Compact structure-of-arrays storage for one weighted-LCS objective table.
///
/// `LcsObjective` is a convenient public value type, but its natural alignment
/// gives each vector element four bytes of padding on 64-bit targets.  The
/// certifier retains two quadratic objective tables, so that padding alone can
/// consume hundreds of MiB on otherwise moderate translation units.  Keeping
/// the 32-bit match count and 64-bit owner cost in parallel arrays preserves
/// the exact objective while using the actual twelve payload bytes per state.
class ObjectiveTable {
public:
  void Assign(size_t count) {
    matchedTokenCounts_.assign(count, 0);
    ownerDepthCosts_.assign(count, 0);
  }

  bool Empty() const {
    return matchedTokenCounts_.empty() && ownerDepthCosts_.empty();
  }

  size_t Size() const {
    assert(matchedTokenCounts_.size() == ownerDepthCosts_.size());
    return matchedTokenCounts_.size();
  }

  LcsObjective Get(size_t index) const {
    assert(index < Size());
    return LcsObjective{matchedTokenCounts_[index], ownerDepthCosts_[index]};
  }

  void Set(size_t index, const LcsObjective &objective) {
    assert(index < Size());
    matchedTokenCounts_[index] = objective.matchedTokenCount;
    ownerDepthCosts_[index] = objective.ownerDepthCost;
  }

private:
  std::vector<uint32_t> matchedTokenCounts_;
  std::vector<uint64_t> ownerDepthCosts_;
};

/// Retained exact state for `OptimalTokenAlignmentOracle`.
///
/// Pair facts use bit 0 for exact token equality, bit 1 for occurrence on at
/// least one globally optimal core path, and bit 2 for occurrence on every
/// globally optimal path. The complete forward/suffix objective tables are
/// retained so conditioned window and frontier queries can be answered without
/// recomputing or approximating the global alignment problem.
struct OptimalTokenAlignmentOracle::Storage {
  size_t aTokenCount = 0;
  size_t bTokenCount = 0;
  size_t stride = 0;
  LcsObjective globalObjective;
  std::vector<uint32_t> ownerDepthGap;
  ObjectiveTable forwardObjectives;
  ObjectiveTable suffixObjectives;
  std::vector<uint8_t> pairFacts;
  uint64_t maxAllocationBytes = 0;
  uint64_t retainedAllocationBytes = 0;
};

namespace {

constexpr uint8_t TokensEqualFact = 1U << 0;
constexpr uint8_t PairOccursOnOptimalPathFact = 1U << 1;
constexpr uint8_t PairIsForcedFact = 1U << 2;

/// Checked heap-payload plan for the complete certified weighted-LCS path.
///
/// `retainedBytes` remains live after certification and is therefore charged
/// against later oracle queries. `constructionPeakBytes` additionally includes
/// the immediate-dominator and dominates-sink arrays used only while forced
/// pairs are certified. The plan is computed before any quadratic allocation.
struct CertifiedLcsAllocationPlan {
  size_t stateCount = 0;
  size_t pairCount = 0;
  uint64_t retainedBytes = 0;
  uint64_t constructionPeakBytes = 0;
};

static bool multiplySizeChecked(size_t lhs, size_t rhs, size_t &out) {
  if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
    return false;
  out = lhs * rhs;
  return true;
}

static bool addBytesChecked(uint64_t value, uint64_t &total) {
  if (total > std::numeric_limits<uint64_t>::max() - value)
    return false;
  total += value;
  return true;
}

template <typename T>
static bool arrayBytesChecked(size_t count, uint64_t &bytes) {
  static_assert(sizeof(size_t) <= sizeof(uint64_t),
                "LCS byte accounting requires size_t to fit in uint64_t");
  if (count > std::vector<T>().max_size() ||
      count > std::numeric_limits<uint64_t>::max() / sizeof(T))
    return false;
  bytes = static_cast<uint64_t>(count) * sizeof(T);
  return true;
}

template <typename T>
static bool addArrayBytesChecked(size_t count, uint64_t &total) {
  uint64_t bytes = 0;
  return arrayBytesChecked<T>(count, bytes) && addBytesChecked(bytes, total);
}

/// Add the exact payload of one compact objective table.
static bool addObjectiveTableBytesChecked(size_t count, uint64_t &total) {
  return addArrayBytesChecked<uint32_t>(count, total) &&
         addArrayBytesChecked<uint64_t>(count, total);
}

static bool gridStateCountChecked(size_t aWidth, size_t bWidth,
                                  size_t &stateCount) {
  if (aWidth == std::numeric_limits<size_t>::max() ||
      bWidth == std::numeric_limits<size_t>::max())
    return false;
  return multiplySizeChecked(aWidth + 1, bWidth + 1, stateCount);
}

static bool hasBoundaryVectorSize(size_t tokenCount, size_t boundaryCount) {
  return tokenCount != std::numeric_limits<size_t>::max() &&
         boundaryCount == tokenCount + 1;
}

static bool buildCertifiedLcsAllocationPlan(
    size_t aTokenCount, size_t bTokenCount,
    size_t ownerDepthGapCopyCount, CertifiedLcsAllocationPlan &plan) {
  plan = CertifiedLcsAllocationPlan{};
  if (!gridStateCountChecked(aTokenCount, bTokenCount, plan.stateCount) ||
      !multiplySizeChecked(aTokenCount, bTokenCount, plan.pairCount))
    return false;

  uint64_t retainedBytes = 0;
  if (!addObjectiveTableBytesChecked(plan.stateCount, retainedBytes) ||
      !addObjectiveTableBytesChecked(plan.stateCount, retainedBytes) ||
      !addArrayBytesChecked<uint8_t>(plan.pairCount, retainedBytes) ||
      !addArrayBytesChecked<int64_t>(aTokenCount, retainedBytes) ||
      !addArrayBytesChecked<int64_t>(aTokenCount, retainedBytes) ||
      !addArrayBytesChecked<LcsAnchorProof>(aTokenCount, retainedBytes))
    return false;
  for (size_t copy = 0; copy < ownerDepthGapCopyCount; ++copy) {
    if (!addArrayBytesChecked<uint32_t>(aTokenCount + 1, retainedBytes))
      return false;
  }
  plan.retainedBytes = retainedBytes;

  // The compact certifier stores flattened dominator predecessors in 32 bits.
  // Under the default one-GiB budget this bound is automatic; callers that
  // raise the budget still fail closed rather than silently truncating an
  // index. Forced match edges are recovered by walking the sink's immediate-
  // dominator chain directly, so no separate state-membership bitset is
  // allocated.
  if (plan.stateCount > std::numeric_limits<uint32_t>::max())
    return false;

  uint64_t constructionPeakBytes = retainedBytes;
  if (!addArrayBytesChecked<uint32_t>(plan.stateCount,
                                      constructionPeakBytes))
    return false;
  plan.constructionPeakBytes = constructionPeakBytes;
  return true;
}

/// Return true when one oracle query's retained and temporary arrays fit.
///
/// This helper is deliberately allocation-only. It does not alter the sparse
/// first-match cache or enumeration traversal used by the fixed6 baseline.
static bool oracleQueryFitsByteBudget(
    const OptimalTokenAlignmentOracle::Storage &storage, size_t aWidth,
    size_t bWidth, uint64_t additionalBytes = 0) {
  size_t stateCount = 0;
  uint64_t requiredBytes = storage.retainedAllocationBytes;
  return gridStateCountChecked(aWidth, bWidth, stateCount) &&
         addObjectiveTableBytesChecked(stateCount, requiredBytes) &&
         addObjectiveTableBytesChecked(stateCount, requiredBytes) &&
         addBytesChecked(additionalBytes, requiredBytes) &&
         requiredBytes <= storage.maxAllocationBytes;
}

inline bool isCoreBetter(unsigned candLen, uint64_t candCost,
                         unsigned bestLen, uint64_t bestCost);
static bool addCostChecked(uint64_t base, uint32_t extra, uint64_t &out);

/// Add two weighted-LCS objective fragments without allowing either component
/// to wrap. The core objective is additive across concatenated path segments:
/// matched-token counts add, as do owner-gap costs.
static bool addObjectivesChecked(const LcsObjective &lhs,
                                 const LcsObjective &rhs,
                                 LcsObjective &out) {
  if (lhs.matchedTokenCount >
      std::numeric_limits<uint32_t>::max() - rhs.matchedTokenCount)
    return false;
  if (lhs.ownerDepthCost >
      std::numeric_limits<uint64_t>::max() - rhs.ownerDepthCost)
    return false;
  out.matchedTokenCount = lhs.matchedTokenCount + rhs.matchedTokenCount;
  out.ownerDepthCost = lhs.ownerDepthCost + rhs.ownerDepthCost;
  return true;
}

/// Subtract two already-disjoint path fragments from one exact total.
static bool subtractObjectivesChecked(const LcsObjective &total,
                                      const LcsObjective &prefix,
                                      const LcsObjective &suffix,
                                      LcsObjective &out) {
  LcsObjective excluded;
  if (!addObjectivesChecked(prefix, suffix, excluded) ||
      excluded.matchedTokenCount > total.matchedTokenCount ||
      excluded.ownerDepthCost > total.ownerDepthCost)
    return false;
  out.matchedTokenCount = total.matchedTokenCount - excluded.matchedTokenCount;
  out.ownerDepthCost = total.ownerDepthCost - excluded.ownerDepthCost;
  return true;
}

/// Return true when the exact sum of two objective fragments equals `target`.
static bool objectiveSumEquals(const LcsObjective &lhs,
                               const LcsObjective &rhs,
                               const LcsObjective &target) {
  LcsObjective sum;
  return addObjectivesChecked(lhs, rhs, sum) && sum == target;
}

/// Return true when the exact sum of three objective fragments equals
/// `target`.
static bool objectiveSumEquals(const LcsObjective &first,
                               const LcsObjective &second,
                               const LcsObjective &third,
                               const LcsObjective &target) {
  LcsObjective prefix;
  return addObjectivesChecked(first, second, prefix) &&
         objectiveSumEquals(prefix, third, target);
}

/// Exact dynamic-programming state for one oracle window.
struct OracleWindowDp {
  size_t aWidth = 0;
  size_t bWidth = 0;
  size_t stride = 0;
  ObjectiveTable forward;
  ObjectiveTable suffix;
};

/// Build the same weighted-LCS objective used by the global certifier, but for
/// one absolute A/B subwindow. `tokensEqual` is queried with absolute token
/// indices, and owner-gap charges retain their original absolute A positions.
template <typename TokensEqualFn>
static bool buildWeightedWindowDp(size_t aBegin, size_t aEnd, size_t bBegin,
                                  size_t bEnd,
                                  ArrayRef<uint32_t> ownerDepthGap,
                                  TokensEqualFn tokensEqual,
                                  OracleWindowDp &out) {
  if (aBegin > aEnd || bBegin > bEnd || aEnd >= ownerDepthGap.size())
    return false;

  const size_t n = aEnd - aBegin;
  const size_t m = bEnd - bBegin;
  out = OracleWindowDp{};
  out.aWidth = n;
  out.bWidth = m;
  if (m == std::numeric_limits<size_t>::max())
    return false;
  out.stride = m + 1;
  size_t stateCount = 0;
  uint64_t ignoredBytes = 0;
  if (!gridStateCountChecked(n, m, stateCount) ||
      !addObjectiveTableBytesChecked(stateCount, ignoredBytes))
    return false;
  out.forward.Assign(stateCount);
  out.suffix.Assign(stateCount);

  auto idx = [&](size_t i, size_t j) -> size_t {
    return i * out.stride + j;
  };
  auto better = [](const LcsObjective &candidate,
                   const LcsObjective &best) {
    return isCoreBetter(candidate.matchedTokenCount,
                        candidate.ownerDepthCost, best.matchedTokenCount,
                        best.ownerDepthCost);
  };

  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      if (i == 0 && j == 0)
        continue;

      LcsObjective best{0, std::numeric_limits<uint64_t>::max()};
      if (i > 0 && j > 0 &&
          tokensEqual(aBegin + i - 1, bBegin + j - 1)) {
        LcsObjective candidate = out.forward.Get(idx(i - 1, j - 1));
        ++candidate.matchedTokenCount;
        if (better(candidate, best))
          best = candidate;
      }
      if (i > 0) {
        LcsObjective candidate = out.forward.Get(idx(i - 1, j));
        if (addCostChecked(candidate.ownerDepthCost,
                           ownerDepthGap[aBegin + i],
                           candidate.ownerDepthCost) &&
            better(candidate, best))
          best = candidate;
      }
      if (j > 0) {
        LcsObjective candidate = out.forward.Get(idx(i, j - 1));
        if (addCostChecked(candidate.ownerDepthCost,
                           ownerDepthGap[aBegin + i],
                           candidate.ownerDepthCost) &&
            better(candidate, best))
          best = candidate;
      }
      out.forward.Set(idx(i, j), best);
    }
  }

  for (size_t ii = n + 1; ii > 0; --ii) {
    const size_t i = ii - 1;
    for (size_t jj = m + 1; jj > 0; --jj) {
      const size_t j = jj - 1;
      if (i == n && j == m)
        continue;

      LcsObjective best{0, std::numeric_limits<uint64_t>::max()};
      if (i < n && j < m && tokensEqual(aBegin + i, bBegin + j)) {
        LcsObjective candidate = out.suffix.Get(idx(i + 1, j + 1));
        ++candidate.matchedTokenCount;
        if (better(candidate, best))
          best = candidate;
      }
      if (i < n) {
        LcsObjective candidate = out.suffix.Get(idx(i + 1, j));
        if (addCostChecked(candidate.ownerDepthCost,
                           ownerDepthGap[aBegin + i + 1],
                           candidate.ownerDepthCost) &&
            better(candidate, best))
          best = candidate;
      }
      if (j < m) {
        LcsObjective candidate = out.suffix.Get(idx(i, j + 1));
        if (addCostChecked(candidate.ownerDepthCost,
                           ownerDepthGap[aBegin + i],
                           candidate.ownerDepthCost) &&
            better(candidate, best))
          best = candidate;
      }
      out.suffix.Set(idx(i, j), best);
    }
  }

  return true;
}

static bool oracleBoundsAreValid(
    const OptimalTokenAlignmentOracle::Storage &storage, uint64_t aBegin,
    uint64_t aEnd, uint64_t bBegin, uint64_t bEnd) {
  return aBegin <= aEnd && bBegin <= bEnd &&
         aEnd <= storage.aTokenCount && bEnd <= storage.bTokenCount;
}

static bool oracleTokensEqual(
    const OptimalTokenAlignmentOracle::Storage &storage, size_t aToken,
    size_t bToken) {
  if (aToken >= storage.aTokenCount || bToken >= storage.bTokenCount)
    return false;
  return (storage.pairFacts[aToken * storage.bTokenCount + bToken] &
          TokensEqualFact) != 0;
}

static bool buildOracleWindowDp(
    const OptimalTokenAlignmentOracle::Storage &storage, uint64_t aBegin,
    uint64_t aEnd, uint64_t bBegin, uint64_t bEnd, OracleWindowDp &out,
    uint64_t additionalBytes = 0) {
  if (!oracleBoundsAreValid(storage, aBegin, aEnd, bBegin, bEnd))
    return false;
  const size_t aWidth = static_cast<size_t>(aEnd - aBegin);
  const size_t bWidth = static_cast<size_t>(bEnd - bBegin);
  if (!oracleQueryFitsByteBudget(storage, aWidth, bWidth, additionalBytes)) {
    REFOLD_LOG_WARN(
        "lcs/oracle",
        "oracle query unavailable: checked aggregate allocation exceeds the "
        "byte budget (aWidth={0}, bWidth={1}, maxBytes={2})",
        aWidth, bWidth, storage.maxAllocationBytes);
    return false;
  }
  return buildWeightedWindowDp(
      static_cast<size_t>(aBegin), static_cast<size_t>(aEnd),
      static_cast<size_t>(bBegin), static_cast<size_t>(bEnd),
      storage.ownerDepthGap,
      [&](size_t aToken, size_t bToken) {
        return oracleTokensEqual(storage, aToken, bToken);
      },
      out);
}

static bool oracleWindowOccursOnOptimalPath(
    const OptimalTokenAlignmentOracle::Storage &storage, uint64_t aBegin,
    uint64_t aEnd, uint64_t bBegin, uint64_t bEnd,
    const OracleWindowDp &window) {
  if (storage.forwardObjectives.Empty() || storage.suffixObjectives.Empty())
    return false;
  const size_t start = static_cast<size_t>(aBegin) * storage.stride +
                       static_cast<size_t>(bBegin);
  const size_t end = static_cast<size_t>(aEnd) * storage.stride +
                     static_cast<size_t>(bEnd);
  const LcsObjective windowObjective =
      window.forward.Get(window.aWidth * window.stride + window.bWidth);
  return objectiveSumEquals(storage.forwardObjectives.Get(start),
                            windowObjective,
                            storage.suffixObjectives.Get(end),
                            storage.globalObjective);
}

} // namespace

bool OptimalTokenAlignmentOracle::HasCompleteCertification() const {
  return static_cast<bool>(storage_);
}

size_t OptimalTokenAlignmentOracle::GetATokenCount() const {
  return storage_ ? storage_->aTokenCount : 0;
}

size_t OptimalTokenAlignmentOracle::GetBTokenCount() const {
  return storage_ ? storage_->bTokenCount : 0;
}

bool CertifiedLcsResult::AnchorBelongsToCertifiedWindow(
    uint64_t aToken, uint64_t bToken) const {
  return std::any_of(
      certificationWindows.begin(), certificationWindows.end(),
      [&](const LcsCertificationWindow &window) {
        return window.ContainsAnchor(aToken, bToken);
      });
}

bool CertifiedLcsResult::CertificationPartitionIsWellFormed(
    uint64_t aTokenCount, uint64_t bTokenCount) const {
  if (certificationWindows.empty())
    return false;
  if (certifiedBoundaries.size() != certificationWindows.size() - 1)
    return false;

  uint64_t expectedABegin = 0;
  uint64_t expectedBBegin = 0;
  bool everyWindowCertified = true;
  for (size_t windowIndex = 0;
       windowIndex < certificationWindows.size(); ++windowIndex) {
    const LcsCertificationWindow &window =
        certificationWindows[windowIndex];
    if (window.aBegin != expectedABegin || window.bBegin != expectedBBegin ||
        window.aBegin > window.aEnd || window.bBegin > window.bEnd ||
        window.aEnd > aTokenCount || window.bEnd > bTokenCount)
      return false;

    const bool makesProgress = window.aBegin != window.aEnd ||
                               window.bBegin != window.bEnd;
    const bool isSingleEmptyStreamWindow =
        certificationWindows.size() == 1 && aTokenCount == 0 &&
        bTokenCount == 0;
    if (!makesProgress && !isSingleEmptyStreamWindow)
      return false;

    everyWindowCertified &= window.IsCertified();
    expectedABegin = window.aEnd;
    expectedBBegin = window.bEnd;

    if (windowIndex + 1 == certificationWindows.size())
      continue;
    const LcsCertifiedBoundary &boundary =
        certifiedBoundaries[windowIndex];
    if (!boundary.IsAuthorized() || boundary.aBoundary != window.aEnd ||
        boundary.bBoundary != window.bEnd)
      return false;
  }

  return expectedABegin == aTokenCount && expectedBBegin == bTokenCount &&
         allWindowsCertified == everyWindowCertified;
}

bool CertifiedLcsResult::HasCompleteGlobalOracle() const {
  if (!globalObjectiveIsExact || !allWindowsCertified ||
      !oracle.HasCompleteCertification() || certificationWindows.size() != 1)
    return false;

  const uint64_t aTokenCount = oracle.GetATokenCount();
  const uint64_t bTokenCount = oracle.GetBTokenCount();
  if (forcedMap.size() != aTokenCount || selectedMap.size() != aTokenCount ||
      selectedAnchorProofs.size() != aTokenCount ||
      !CertificationPartitionIsWellFormed(aTokenCount, bTokenCount))
    return false;

  const LcsCertificationWindow &window = certificationWindows.front();
  return window.IsCertified() && window.aBegin == 0 &&
         window.aEnd == aTokenCount && window.bBegin == 0 &&
         window.bEnd == bTokenCount;
}

bool CertifiedLcsResult::HasCompleteSemanticOracleForWindow(
    size_t windowIndex) const {
  return GetSemanticOracleForWindow(windowIndex) != nullptr;
}

const OptimalTokenAlignmentOracle *
CertifiedLcsResult::GetSemanticOracleForWindow(size_t windowIndex) const {
  // The historical complete-stream oracle answers for the one-window case and
  // keeps that path byte-identical to its previous behavior.
  if (windowIndex == 0 && certificationWindows.size() == 1 &&
      HasCompleteGlobalOracle())
    return &oracle;

  // Otherwise only an explicitly retained per-window oracle authorizes map
  // enumeration. A certified window whose quadratic facts were released
  // authorizes its core-forced anchors and nothing more, so answering from
  // `status` alone would hand a caller pair facts that no longer exist.
  if (windowIndex >= certificationWindows.size() ||
      windowIndex >= windowOracles.size() || !globalObjectiveIsExact)
    return nullptr;

  const LcsCertificationWindow &window = certificationWindows[windowIndex];
  const OptimalTokenAlignmentOracle &windowOracle = windowOracles[windowIndex];
  if (!window.IsCertified() || !windowOracle.HasCompleteCertification() ||
      forcedMap.size() != selectedMap.size() ||
      forcedMap.size() != selectedAnchorProofs.size())
    return nullptr;

  // The oracle is in window-local coordinates, so its extent must equal this
  // window's exact width on both sides. An oracle built for a different
  // rectangle would silently reinterpret every translated query.
  if (windowOracle.GetATokenCount() != window.aEnd - window.aBegin ||
      windowOracle.GetBTokenCount() != window.bEnd - window.bBegin)
    return nullptr;
  return &windowOracle;
}

void CertifiedLcsResult::RetainOnlyCoreForcedAnchors() {
  selectedMap = forcedMap;
  selectedAnchorProofs.assign(forcedMap.size(), LcsAnchorProof{});
  for (size_t aToken = 0; aToken < forcedMap.size(); ++aToken) {
    if (forcedMap[aToken] < 0)
      continue;
    selectedAnchorProofs[aToken] =
        LcsAnchorProof{LcsAnchorProofKind::CoreOptimalPathForced, 0};
  }
}

std::vector<std::string>
describeLcsCertificationRun(
    const CertifiedLcsResult &result,
    const LcsCertificationDiagnosticEvidence &diagnosticEvidence,
    uint64_t aTokenCount, uint64_t bTokenCount) {
  auto boolText = [](bool value) -> StringRef {
    return value ? "true" : "false";
  };
  auto formatBoundaries = [](ArrayRef<uint64_t> boundaries) {
    std::string text;
    raw_string_ostream stream(text);
    stream << '[';
    for (size_t index = 0; index < boundaries.size(); ++index) {
      if (index != 0)
        stream << ',';
      stream << boundaries[index];
    }
    stream << ']';
    stream.flush();
    return text;
  };

  std::vector<std::string> lines;
  lines.reserve(4 + diagnosticEvidence.boundaryFrontierProjections.size() +
                result.certifiedBoundaries.size() +
                result.certificationWindows.size() * 2);

  lines.push_back(
      formatv("global A=[0,{0}) B=[0,{1})", aTokenCount, bTokenCount).str());
  lines.push_back(
      formatv("global objective exact={0} matchedTokens={1} "
              "ownerDepthCost={2}",
              boolText(result.globalObjectiveIsExact),
              result.globalObjective.matchedTokenCount,
              result.globalObjective.ownerDepthCost)
          .str());
  lines.push_back(
      formatv("candidate A boundaries={0}",
              formatBoundaries(diagnosticEvidence.candidateABoundaries))
          .str());

  for (const LcsBoundaryFrontierProjection &projection :
       diagnosticEvidence.boundaryFrontierProjections) {
    lines.push_back(
        formatv("boundary A={0} objectiveExact={1} frontiers={2}",
                projection.aBoundary, boolText(projection.objectiveIsExact),
                formatBoundaries(projection.admissibleBFrontiers))
            .str());
  }

  for (size_t index = 0; index < result.certifiedBoundaries.size(); ++index) {
    const LcsCertifiedBoundary &boundary = result.certifiedBoundaries[index];
    lines.push_back(
        formatv("seam index={0} A={1} B={2} proof={3}", index,
                boundary.aBoundary, boundary.bBoundary,
                toString(boundary.proofKind))
            .str());
  }

  uint64_t retainedAnchors = 0;
  uint64_t coreForcedAnchors = 0;
  uint64_t semanticAnchors = 0;
  uint64_t ownerAlignedSlideAnchors = 0;
  for (size_t aToken = 0; aToken < result.selectedMap.size(); ++aToken) {
    if (result.selectedMap[aToken] < 0)
      continue;
    ++retainedAnchors;
    if (aToken >= result.selectedAnchorProofs.size())
      continue;
    switch (result.selectedAnchorProofs[aToken].kind) {
    case LcsAnchorProofKind::None:
      break;
    case LcsAnchorProofKind::CoreOptimalPathForced:
      ++coreForcedAnchors;
      break;
    case LcsAnchorProofKind::EquivalentNormalizedHunkAndOwner:
      ++semanticAnchors;
      break;
    case LcsAnchorProofKind::OwnerAlignedDeletionSlide:
      ++ownerAlignedSlideAnchors;
      break;
    }
  }
  // Every retained anchor is reported under the proof that authorized it, so
  // the named counts sum to the total. An anchor counted in the total and in
  // no kind would read as a census of forced anchors that silently omits the
  // repaired ones.
  lines.push_back(
      formatv("retained anchors total={0} CoreOptimalPathForced={1} "
              "EquivalentNormalizedHunkAndOwner={2} "
              "OwnerAlignedDeletionSlide={3}",
              retainedAnchors, coreForcedAnchors, semanticAnchors,
              ownerAlignedSlideAnchors)
          .str());

  for (size_t index = 0; index < result.certificationWindows.size(); ++index) {
    const LcsCertificationWindow &window = result.certificationWindows[index];
    lines.push_back(
        formatv("window index={0} A=[{1},{2}) B=[{3},{4}) "
                "requiredBytes={5} proofBudgetBytes={6} status={7}",
                index, window.aBegin, window.aEnd, window.bBegin, window.bEnd,
                window.requiredBytes, window.proofBudgetBytes,
                toString(window.status))
            .str());
    if (!window.IsCertified()) {
      lines.push_back(
          formatv("failed rectangle index={0} A=[{1},{2}) B=[{3},{4}) "
                  "requiredBytes={5} proofBudgetBytes={6} status={7}",
                  index, window.aBegin, window.aEnd, window.bBegin,
                  window.bEnd, window.requiredBytes, window.proofBudgetBytes,
                  toString(window.status))
              .str());
    }
  }
  lines.push_back(
      formatv("all windows certified={0}",
              boolText(result.allWindowsCertified))
          .str());
  return lines;
}

bool OptimalTokenAlignmentOracle::PairOccursOnOptimalPath(
    uint64_t aToken, uint64_t bToken) const {
  if (!storage_ || aToken >= storage_->aTokenCount ||
      bToken >= storage_->bTokenCount)
    return false;
  return (storage_->pairFacts[static_cast<size_t>(aToken) *
                                  storage_->bTokenCount +
                              static_cast<size_t>(bToken)] &
          PairOccursOnOptimalPathFact) != 0;
}

LcsObjective OptimalTokenAlignmentOracle::ObjectiveForWindow(
    uint64_t aBegin, uint64_t aEnd, uint64_t bBegin, uint64_t bEnd) const {
  OracleWindowDp window;
  if (!storage_ ||
      !buildOracleWindowDp(*storage_, aBegin, aEnd, bBegin, bEnd, window))
    return LcsObjective{};
  return window.forward.Get(window.aWidth * window.stride + window.bWidth);
}

OptimalLcsMapEnumeration
OptimalTokenAlignmentOracle::EnumerateOptimalMapsForWindow(
    uint64_t aWindowBegin, uint64_t aWindowEnd, uint64_t bWindowBegin,
    uint64_t bWindowEnd, size_t maxUniqueMaps) const {
  OptimalLcsMapEnumeration result;
  if (!storage_ || maxUniqueMaps == 0 ||
      !oracleBoundsAreValid(*storage_, aWindowBegin, aWindowEnd, bWindowBegin,
                            bWindowEnd))
    return result;

  using MatchEdge = std::pair<uint64_t, uint64_t>;
  using MatchSequence = std::vector<MatchEdge>;
  struct EnumerationFrame {
    size_t localA = 0;
    size_t localB = 0;
    const std::vector<MatchEdge> *nextMatches = nullptr;
    size_t nextMatchIndex = 0;
    bool ownsIncomingMatch = false;
  };

  const size_t aWidth = static_cast<size_t>(aWindowEnd - aWindowBegin);
  const size_t bWidth = static_cast<size_t>(bWindowEnd - bWindowBegin);
  const size_t maxMatchCount = std::min(aWidth, bWidth);
  size_t stateCount = 0;
  size_t sequenceEdgeLimit = 0;
  size_t resultMapElementLimit = 0;
  uint64_t additionalBytes = 0;
  if (!gridStateCountChecked(aWidth, bWidth, stateCount) ||
      maxUniqueMaps == std::numeric_limits<size_t>::max() ||
      !multiplySizeChecked(maxUniqueMaps + 1, maxMatchCount,
                           sequenceEdgeLimit) ||
      !multiplySizeChecked(maxUniqueMaps, aWidth, resultMapElementLimit) ||
      !addArrayBytesChecked<uint32_t>(stateCount, additionalBytes) ||
      !addArrayBytesChecked<std::pair<size_t, size_t>>(stateCount,
                                                       additionalBytes) ||
      !addArrayBytesChecked<EnumerationFrame>(maxMatchCount + 1,
                                              additionalBytes) ||
      !addArrayBytesChecked<MatchEdge>(maxMatchCount, additionalBytes) ||
      !addArrayBytesChecked<MatchSequence>(maxUniqueMaps + 1,
                                           additionalBytes) ||
      !addArrayBytesChecked<MatchEdge>(sequenceEdgeLimit, additionalBytes) ||
      !addArrayBytesChecked<std::vector<int64_t>>(maxUniqueMaps,
                                                  additionalBytes) ||
      !addArrayBytesChecked<int64_t>(resultMapElementLimit,
                                     additionalBytes))
    return result;

  OracleWindowDp window;
  if (!buildOracleWindowDp(*storage_, aWindowBegin, aWindowEnd, bWindowBegin,
                           bWindowEnd, window, additionalBytes) ||
      !oracleWindowOccursOnOptimalPath(*storage_, aWindowBegin, aWindowEnd,
                                       bWindowBegin, bWindowEnd, window))
    return result;

  auto idx = [&](size_t localA, size_t localB) {
    return localA * window.stride + localB;
  };

  // A zero-match optimum has exactly one distinct match map regardless of how
  // many insertion/deletion interleavings realize it.  Handle it directly so
  // very long pure gaps never consume traversal stack or proof budget.
  if (window.suffix.Get(0).matchedTokenCount == 0) {
    result.maps.emplace_back(window.aWidth, -1);
    result.complete = true;
    return result;
  }

  // Return every match edge that can be the first match of an optimal suffix
  // beginning at `start`.  Gap-only optimal transitions are traversed
  // iteratively and collapsed, so different insertion/deletion interleavings
  // reaching the same match edge do not manufacture duplicate match maps.
  std::map<size_t, std::vector<MatchEdge>> firstMatchCache;
  std::vector<uint32_t> visitEpoch(stateCount, 0);
  uint32_t nextEpoch = 0;
  std::vector<std::pair<size_t, size_t>> gapStack;

  auto firstOptimalMatches = [&](size_t startA, size_t startB)
      -> const std::vector<MatchEdge> & {
    const size_t startState = idx(startA, startB);
    auto cacheIt = firstMatchCache.find(startState);
    if (cacheIt != firstMatchCache.end())
      return cacheIt->second;

    if (++nextEpoch == 0) {
      std::fill(visitEpoch.begin(), visitEpoch.end(), 0);
      nextEpoch = 1;
    }
    gapStack.clear();
    gapStack.emplace_back(startA, startB);
    visitEpoch[startState] = nextEpoch;

    std::vector<MatchEdge> matches;
    while (!gapStack.empty()) {
      const auto state = gapStack.back();
      gapStack.pop_back();
      const size_t localA = state.first;
      const size_t localB = state.second;
      const LcsObjective current = window.suffix.Get(idx(localA, localB));

      if (localA < window.aWidth && localB < window.bWidth &&
          oracleTokensEqual(*storage_, aWindowBegin + localA,
                            bWindowBegin + localB)) {
        const LcsObjective edge{1, 0};
        const LcsObjective tail =
            window.suffix.Get(idx(localA + 1, localB + 1));
        if (objectiveSumEquals(edge, tail, current)) {
          matches.emplace_back(aWindowBegin + localA,
                               bWindowBegin + localB);
        }
      }

      auto addGapSuccessor = [&](size_t nextA, size_t nextB,
                                 uint32_t gapCost) {
        const size_t nextState = idx(nextA, nextB);
        if (visitEpoch[nextState] == nextEpoch)
          return;
        const LcsObjective edge{0, gapCost};
        if (!objectiveSumEquals(edge, window.suffix.Get(nextState), current))
          return;
        visitEpoch[nextState] = nextEpoch;
        gapStack.emplace_back(nextA, nextB);
      };

      if (localA < window.aWidth) {
        addGapSuccessor(
            localA + 1, localB,
            storage_->ownerDepthGap[aWindowBegin + localA + 1]);
      }
      if (localB < window.bWidth) {
        addGapSuccessor(localA, localB + 1,
                        storage_->ownerDepthGap[aWindowBegin + localA]);
      }
    }

    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return firstMatchCache.emplace(startState, std::move(matches))
        .first->second;
  };

  std::vector<EnumerationFrame> frames;
  MatchSequence sequence;
  auto pushFrame = [&](size_t localA, size_t localB,
                       bool ownsIncomingMatch) -> bool {
    const LcsObjective remaining = window.suffix.Get(idx(localA, localB));
    const std::vector<MatchEdge> *nextMatches = nullptr;
    if (remaining.matchedTokenCount != 0) {
      const std::vector<MatchEdge> &matches =
          firstOptimalMatches(localA, localB);
      if (matches.empty())
        return false;
      nextMatches = &matches;
    }
    frames.push_back(EnumerationFrame{localA, localB, nextMatches, 0,
                                      ownsIncomingMatch});
    return true;
  };
  auto popFrame = [&]() {
    const bool ownsIncomingMatch = frames.back().ownsIncomingMatch;
    frames.pop_back();
    if (ownsIncomingMatch)
      sequence.pop_back();
  };

  if (!pushFrame(0, 0, /*ownsIncomingMatch=*/false))
    return result;

  std::vector<MatchSequence> sequences;
  while (!frames.empty()) {
    EnumerationFrame &frame = frames.back();
    if (!frame.nextMatches) {
      sequences.push_back(sequence);
      if (sequences.size() > maxUniqueMaps)
        return result;
      popFrame();
      continue;
    }

    if (frame.nextMatchIndex >= frame.nextMatches->size()) {
      popFrame();
      continue;
    }

    const MatchEdge edge = (*frame.nextMatches)[frame.nextMatchIndex++];
    const size_t edgeLocalA =
        static_cast<size_t>(edge.first - aWindowBegin);
    const size_t edgeLocalB =
        static_cast<size_t>(edge.second - bWindowBegin);
    sequence.push_back(edge);
    if (!pushFrame(edgeLocalA + 1, edgeLocalB + 1,
                   /*ownsIncomingMatch=*/true))
      return result;
  }

  result.maps.reserve(sequences.size());
  for (const MatchSequence &matchSequence : sequences) {
    std::vector<int64_t> map(window.aWidth, -1);
    for (const MatchEdge &edge : matchSequence) {
      const size_t localA = static_cast<size_t>(edge.first - aWindowBegin);
      map[localA] = static_cast<int64_t>(edge.second);
    }
    result.maps.push_back(std::move(map));
  }
  result.complete = true;
  return result;
}

namespace {
/// Quadratic table family selected by one public LCS overload.
enum class QuadraticLcsAllocationKind { WeightedMap, PlainMap };

/// Return true when the exact quadratic implementation would exceed the
/// configured aggregate byte budget and the caller must use exact Hirschberg.
static bool shouldUseLinearSpace(size_t n, size_t m, uint64_t maxBytes,
                                 QuadraticLcsAllocationKind kind) {
  size_t stateCount = 0;
  uint64_t requiredBytes = 0;
  const bool dimensionsValid = gridStateCountChecked(n, m, stateCount);
  const bool allocationValid =
      dimensionsValid &&
      (kind == QuadraticLcsAllocationKind::WeightedMap
           ? addArrayBytesChecked<uint32_t>(stateCount, requiredBytes) &&
                 addArrayBytesChecked<uint64_t>(stateCount, requiredBytes)
           : addArrayBytesChecked<unsigned>(stateCount, requiredBytes)) &&
      addArrayBytesChecked<int64_t>(n, requiredBytes);
  if (allocationValid && requiredBytes <= maxBytes)
    return false;

  REFOLD_LOG_WARN(
      "lcs/map",
      "hirschberg fallback: checked quadratic allocation exceeds the byte "
      "budget (kind={0}, n={1}, m={2}, states={3}, requiredBytes={4}, "
      "maxBytes={5})",
      kind == QuadraticLcsAllocationKind::WeightedMap ? "weighted" : "plain",
      n, m, dimensionsValid ? stateCount : 0,
      allocationValid ? requiredBytes : 0, maxBytes);
  return true;
}

/// Compare structural-LCS candidates: longer subsequence first, then lower
/// owner-depth cost.
inline bool isCoreBetter(unsigned candLen, std::uint64_t candCost,
                         unsigned bestLen, std::uint64_t bestCost) {
  if (candLen != bestLen)
    return candLen > bestLen;
  return candCost < bestCost;
}

/// Add \p extra to \p base without allowing unsigned wraparound.
static bool addCostChecked(uint64_t base, uint32_t extra, uint64_t &out) {
  if (base > std::numeric_limits<uint64_t>::max() - extra)
    return false;
  out = base + extra;
  return true;
}

/// The compact internal table stores the public objective's two exact scalar
/// components without per-element alignment padding.

/// Compute forward and suffix DP tables for the structural LCS core objective.
///
/// The suffix table uses the same transition costs as the forward table:
/// deleting A[i] pays ownerDepthGap[i + 1], and inserting B[j] at the current
/// A gap pays ownerDepthGap[i]. Keeping those costs identical makes the
/// admissibility check a real certificate for the same objective used to build
/// the production map.
static bool buildCoreLcsDpTables(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                                 ArrayRef<uint32_t> ownerDepthGap,
                                 ObjectiveTable &forward,
                                 ObjectiveTable &suffix,
                                 size_t &stride) {
  OracleWindowDp window;
  if (!buildWeightedWindowDp(
          0, a.size(), 0, b.size(), ownerDepthGap,
          [&](size_t aToken, size_t bToken) {
            return a[aToken] == b[bToken];
          },
          window))
    return false;

  stride = window.stride;
  forward = std::move(window.forward);
  suffix = std::move(window.suffix);
  return true;
}

/// Mark exact match edges that occur on every optimal path through one DP box.
///
/// The optimal transitions form a monotone DAG over DP states `(i,j)`. A match
/// edge is forced exactly when every source-to-sink path reaches its source
/// state and that state has no other globally optimal outgoing transition.
/// The first condition is computed with exact DAG dominators; the second
/// excludes paths that pass through the same state but choose deletion or
/// insertion instead of the match. This avoids the incorrect shortcut that a
/// pair is forced merely because each token has one individually admissible
/// partner.
static void markForcedOptimalPairs(
    size_t n, size_t m, size_t stride, ArrayRef<uint32_t> ownerDepthGap,
    const ObjectiveTable &forward, const ObjectiveTable &suffix,
    const LcsObjective &total, std::vector<uint8_t> &pairFacts,
    uint64_t bTokenOffset, std::vector<int64_t> &forcedMap) {
  const size_t stateCount = (n + 1) * (m + 1);
  assert(stateCount <= std::numeric_limits<uint32_t>::max() &&
         "certified allocation preflight must bound dominator indices");
  const uint32_t invalid = std::numeric_limits<uint32_t>::max();
  auto idx = [&](size_t i, size_t j) -> size_t { return i * stride + j; };

  auto transitionOccursOnOptimalPath =
      [&](size_t fromI, size_t fromJ, size_t toI, size_t toJ,
          uint32_t matchedTokenCount, uint64_t ownerDepthCost) {
        return objectiveSumEquals(
            forward.Get(idx(fromI, fromJ)),
            LcsObjective{matchedTokenCount, ownerDepthCost},
            suffix.Get(idx(toI, toJ)), total);
      };

  // Row-major DP-state order is topological: every insertion, deletion, and
  // match strictly increases the flattened state index. Immediate dominators
  // can therefore be computed in one pass by intersecting already-complete
  // predecessor dominator chains.
  std::vector<uint32_t> immediateDominator(stateCount, invalid);
  immediateDominator[0] = 0;
  auto intersectDominators = [&](uint32_t lhs, uint32_t rhs) {
    while (lhs != rhs) {
      while (lhs > rhs)
        lhs = immediateDominator[lhs];
      while (rhs > lhs)
        rhs = immediateDominator[rhs];
    }
    return lhs;
  };

  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      const size_t state = idx(i, j);
      if (state == 0)
        continue;

      // States outside the conditioned optimal source-to-sink DAG cannot
      // dominate the sink and receive no predecessor in the original
      // formulation below. Reject them with one exact prefix/suffix test
      // instead of evaluating all three candidate transitions. This preserves
      // the same dominator graph while avoiding most transition work for the
      // common narrow-optimal-corridor case.
      if (!objectiveSumEquals(forward.Get(state), suffix.Get(state), total))
        continue;

      uint32_t dominator = invalid;
      auto addPredecessor = [&](size_t predecessorIndex) {
        const uint32_t predecessor = static_cast<uint32_t>(predecessorIndex);
        if (immediateDominator[predecessor] == invalid)
          return;
        dominator = dominator == invalid
                        ? predecessor
                        : intersectDominators(dominator, predecessor);
      };

      if (i > 0 && j > 0 &&
          (pairFacts[(i - 1) * m + (j - 1)] &
           PairOccursOnOptimalPathFact) != 0)
        addPredecessor(idx(i - 1, j - 1));
      if (i > 0 && transitionOccursOnOptimalPath(
                       i - 1, j, i, j, 0, ownerDepthGap[i]))
        addPredecessor(idx(i - 1, j));
      if (j > 0 && transitionOccursOnOptimalPath(
                       i, j - 1, i, j, 0, ownerDepthGap[i]))
        addPredecessor(idx(i, j - 1));

      immediateDominator[state] = dominator;
    }
  }

  // Nodes on the sink's immediate-dominator chain occur on every conditioned
  // optimal path. The earlier implementation first copied that chain into a
  // state-count bitset and then rescanned every A/B token pair to find the few
  // chain states that can own a forced match. Walking the chain directly is
  // exactly equivalent: the old final scan accepted a pair iff its source
  // state was on this same chain and its match was the only optimal outgoing
  // transition. The direct walk removes one full O(n*m) pass and the redundant
  // bitset without changing the theorem or iteration-dependent selection.
  const uint32_t sink = static_cast<uint32_t>(idx(n, m));
  if (immediateDominator[sink] == invalid)
    return;

  for (uint32_t state = sink;; state = immediateDominator[state]) {
    const size_t stateIndex = static_cast<size_t>(state);
    const size_t ai = stateIndex / stride;
    const size_t bj = stateIndex % stride;

    if (ai < n && bj < m) {
      uint8_t &facts = pairFacts[ai * m + bj];
      if ((facts & PairOccursOnOptimalPathFact) != 0) {
        uint32_t optimalOutgoingCount = 1; // This pair's match edge.
        if (transitionOccursOnOptimalPath(ai, bj, ai + 1, bj, 0,
                                          ownerDepthGap[ai + 1]))
          ++optimalOutgoingCount;
        if (transitionOccursOnOptimalPath(ai, bj, ai, bj + 1, 0,
                                          ownerDepthGap[ai]))
          ++optimalOutgoingCount;
        if (optimalOutgoingCount == 1) {
          facts |= PairIsForcedFact;
          assert(forcedMap[ai] < 0 &&
                 "one A token cannot have two forced optimal partners");
          forcedMap[ai] =
              static_cast<int64_t>(bTokenOffset + static_cast<uint64_t>(bj));
        }
      }
    }

    if (state == 0)
      break;
  }
}

/// Build the exact core-certified LCS result used by refolding.
///
/// The certifier has one production authority: every selected anchor must occur
/// on every optimal path through the conditioned weighted-LCS window. Ambiguous
/// equal-token pairs remain suppressed. A separate semantic resolver may later
/// restore a representative map only after proving that every remaining
/// explanation induces one equivalent normalized owner/edit realization.
static LcsObjective
lcsObjectiveLinearSpaceWeighted(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                                ArrayRef<uint32_t> ownerDepthGap);

static void initializeWindowCertificationResult(
    uint64_t aBegin, uint64_t aEnd, uint64_t bBegin, uint64_t bEnd,
    unsigned long long maxBytes, LcsWindowCertificationResult &result) {
  result = LcsWindowCertificationResult{};
  result.window = LcsCertificationWindow{
      aBegin, aEnd, bBegin, bEnd,
      LcsWindowCertificationStatus::PartitionUnresolved,
      /*requiredBytes=*/0, static_cast<uint64_t>(maxBytes)};

  const size_t aWidth = static_cast<size_t>(aEnd - aBegin);
  result.forcedMap.assign(aWidth, -1);
  result.selectedMap.assign(aWidth, -1);
  result.selectedAnchorProofs.assign(aWidth, LcsAnchorProof{});
}

/// Complete a failed local proof without selecting one optimal path.
///
/// Hirschberg's linear-space objective is exact, but it does not materialize
/// the all-optimal relation. The maps therefore remain suppressed while the
/// caller still receives the exact conditioned objective and failure rectangle.
static void finishWindowObjectiveLinearSpace(
    ArrayRef<StringRef> aWindow, ArrayRef<StringRef> bWindow,
    ArrayRef<uint32_t> ownerDepthGap,
    LcsWindowCertificationResult &result) {
  result.objective =
      lcsObjectiveLinearSpaceWeighted(aWindow, bWindow, ownerDepthGap);
  result.objectiveIsExact = true;
}

/// Copy the scalar owner-depth objective for one inclusive A-gap range.
static std::vector<uint32_t>
copyOwnerDepthGaps(ArrayRef<LcsAGapProvenance> gapProvenance,
                   size_t aBegin, size_t aEnd) {
  assert(aBegin <= aEnd && aEnd < gapProvenance.size());
  std::vector<uint32_t> ownerDepthGap;
  ownerDepthGap.reserve(aEnd - aBegin + 1);
  for (const LcsAGapProvenance &profile :
       gapProvenance.slice(aBegin, aEnd - aBegin + 1))
    ownerDepthGap.push_back(profile.ownerDepth);
  return ownerDepthGap;
}

/// Canonicalize the evidence-only physical identity request.
static bool diagnosticIdentityLess(
    const LcsProtectedBoundaryDiagnosticIdentity &lhs,
    const LcsProtectedBoundaryDiagnosticIdentity &rhs) {
  if (lhs.identityId != rhs.identityId)
    return lhs.identityId < rhs.identityId;
  return lhs.aBoundary < rhs.aBoundary;
}

/// Canonicalize the caller's evidence-only identity request.
static void canonicalizeDiagnosticIdentityRequests(
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (!diagnosticEvidence)
    return;
  std::sort(diagnosticEvidence->protectedBoundaryIdentities.begin(),
            diagnosticEvidence->protectedBoundaryIdentities.end(),
            diagnosticIdentityLess);
}

/// Clear generated evidence while preserving the caller's identity request.
static void resetCertificationDiagnosticOutputs(
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (!diagnosticEvidence)
    return;
  canonicalizeDiagnosticIdentityRequests(diagnosticEvidence);
  diagnosticEvidence->retainedAdmissiblePairBytes = 0;
  diagnosticEvidence->candidateABoundaries.clear();
  diagnosticEvidence->boundaryFrontierProjections.clear();
  diagnosticEvidence->ambiguityWindows.clear();
}

/// Charge one complete admissible-pair vector against the run-wide ledger.
///
/// The charge is computed before any pair payload is allocated. Failure leaves
/// the ledger unchanged so the owning certified window can publish its exact
/// proof facts with only pair enumeration marked incomplete.
static bool tryChargeAdmissiblePairEvidence(
    size_t pairCount, LcsCertificationDiagnosticEvidence &evidence,
    uint64_t &requiredBytes) {
  requiredBytes = 0;
  if (!arrayBytesChecked<LcsDiagnosticAdmissiblePair>(pairCount,
                                                       requiredBytes)) {
    requiredBytes = std::numeric_limits<uint64_t>::max();
    return false;
  }

  uint64_t chargedBytes = evidence.retainedAdmissiblePairBytes;
  if (!addBytesChecked(requiredBytes, chargedBytes) ||
      chargedBytes > evidence.admissiblePairByteBudget)
    return false;
  evidence.retainedAdmissiblePairBytes = chargedBytes;
  return true;
}

/// Return true when one diagnostic record belongs to a certification window.
static bool diagnosticRecordBelongsToWindow(
    const LcsAmbiguityWindowDiagnosticRecord &record,
    const LcsCertificationWindow &window) {
  if (record.certificationStatus != LcsWindowCertificationStatus::Certified)
    return record.aBegin == window.aBegin && record.aEnd == window.aEnd &&
           record.bBegin == window.bBegin && record.bEnd == window.bEnd &&
           record.certificationStatus == window.status;
  if (!window.IsCertified() || window.aBegin > record.aBegin ||
      record.aEnd > window.aEnd || window.bBegin > record.bBegin ||
      record.bEnd > window.bEnd)
    return false;

  const LcsDiagnosticWindowAnchor &left = record.leftForcedAnchor;
  const bool ownsLeft =
      left.kind == LcsDiagnosticWindowAnchorKind::ForcedToken
          ? window.ContainsAnchor(left.aToken, left.bToken)
          : (left.kind == LcsDiagnosticWindowAnchorKind::StreamBegin ||
             left.kind == LcsDiagnosticWindowAnchorKind::CertifiedBoundary) &&
                window.aBegin == left.aToken &&
                window.bBegin == left.bToken;
  const LcsDiagnosticWindowAnchor &right = record.rightForcedAnchor;
  const bool ownsRight =
      right.kind == LcsDiagnosticWindowAnchorKind::ForcedToken
          ? window.ContainsAnchor(right.aToken, right.bToken)
          : (right.kind == LcsDiagnosticWindowAnchorKind::StreamEnd ||
             right.kind ==
                 LcsDiagnosticWindowAnchorKind::CertifiedBoundary) &&
                window.aEnd == right.aToken && window.bEnd == right.bToken;
  return ownsLeft && ownsRight;
}

/// Canonicalize and audit one complete diagnostic run.
///
/// The collector is observational: an invariant failure is reported and the
/// malformed ambiguity transcript is suppressed, but the certified maps, seams,
/// statuses, and candidate order are never changed. In particular, every failed
/// local certification window must own exactly one status-only record. This
/// rules out the former synthetic full-stream failure record for partitioned
/// runs.
static void finalizeCertificationAmbiguityDiagnostics(
    const CertifiedLcsResult &result, uint64_t aTokenCount,
    uint64_t bTokenCount,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (!diagnosticEvidence)
    return;

  auto &records = diagnosticEvidence->ambiguityWindows;
  std::stable_sort(
      records.begin(), records.end(),
      [](const LcsAmbiguityWindowDiagnosticRecord &lhs,
         const LcsAmbiguityWindowDiagnosticRecord &rhs) {
        return std::tie(lhs.aBegin, lhs.bBegin, lhs.aEnd, lhs.bEnd,
                        lhs.certificationStatus) <
               std::tie(rhs.aBegin, rhs.bBegin, rhs.aEnd, rhs.bEnd,
                        rhs.certificationStatus);
      });

  bool wellFormed = result.CertificationPartitionIsWellFormed(
      aTokenCount, bTokenCount);
  uint64_t retainedPairBytes = 0;
  std::vector<size_t> failedWindowRecordCounts(
      result.certificationWindows.size(), 0);
  for (const LcsAmbiguityWindowDiagnosticRecord &record : records) {
    size_t owningWindow = result.certificationWindows.size();
    for (size_t windowIndex = 0;
         windowIndex < result.certificationWindows.size(); ++windowIndex) {
      if (!diagnosticRecordBelongsToWindow(
              record, result.certificationWindows[windowIndex]))
        continue;
      if (owningWindow != result.certificationWindows.size()) {
        wellFormed = false;
        break;
      }
      owningWindow = windowIndex;
    }
    if (owningWindow == result.certificationWindows.size()) {
      wellFormed = false;
      continue;
    }

    if (record.certificationStatus == LcsWindowCertificationStatus::Certified) {
      // Core certification and optional evidence completeness are independent.
      // An incomplete pair census or boundary projection is valid diagnostic
      // output and must never invalidate the owning production window.
      wellFormed &= record.coreCertificationComplete;
      if (!record.admissiblePairEnumerationComplete)
        wellFormed &= record.admissiblePairs.empty();
      wellFormed &= addArrayBytesChecked<LcsDiagnosticAdmissiblePair>(
          record.admissiblePairs.size(), retainedPairBytes);

      bool everyProjectionComplete = true;
      for (const LcsProtectedBoundaryDiagnosticProjection &projection :
           record.protectedBoundaryProjections) {
        everyProjectionComplete &= projection.projectionComplete;
        if (!projection.projectionComplete)
          wellFormed &= projection.admissibleBFrontiers.empty();
      }
      if (record.boundaryProjectionComplete)
        wellFormed &= everyProjectionComplete;
      continue;
    }

    ++failedWindowRecordCounts[owningWindow];
    wellFormed &= !record.coreCertificationComplete &&
                  !record.admissiblePairEnumerationComplete &&
                  !record.boundaryProjectionComplete &&
                  record.admissiblePairs.empty();
    for (const LcsProtectedBoundaryDiagnosticProjection &projection :
         record.protectedBoundaryProjections) {
      wellFormed &= !projection.projectionComplete &&
                    projection.admissibleBFrontiers.empty();
    }
  }

  wellFormed &= retainedPairBytes ==
                    diagnosticEvidence->retainedAdmissiblePairBytes &&
                retainedPairBytes <=
                    diagnosticEvidence->admissiblePairByteBudget;

  for (size_t windowIndex = 0;
       windowIndex < result.certificationWindows.size(); ++windowIndex) {
    const size_t expected =
        result.certificationWindows[windowIndex].IsCertified() ? 0 : 1;
    wellFormed &= failedWindowRecordCounts[windowIndex] == expected;
  }

  if (wellFormed)
    return;

  REFOLD_LOG_WARN(
      "lcs/diagnostic",
      "discarding malformed ambiguity diagnostics without changing the "
      "certified alignment result");
  records.clear();
  diagnosticEvidence->retainedAdmissiblePairBytes = 0;
}

/// Run-scoped owner of the optional ambiguity evidence ledger.
///
/// Complete-stream and partitioned certification construct this same wrapper,
/// reset generated output exactly once, append every local record through the
/// shared certifier, and finalize the canonical transcript against the completed
/// certification partition. The wrapper allocates nothing when diagnostics are
/// disabled.
class LcsAmbiguityDiagnosticRun {
public:
  LcsAmbiguityDiagnosticRun(
      uint64_t aTokenCount, uint64_t bTokenCount,
      LcsCertificationDiagnosticEvidence *diagnosticEvidence)
      : aTokenCount_(aTokenCount), bTokenCount_(bTokenCount),
        diagnosticEvidence_(diagnosticEvidence) {
    resetCertificationDiagnosticOutputs(diagnosticEvidence_);
  }

  LcsCertificationDiagnosticEvidence *Evidence() const {
    return diagnosticEvidence_;
  }

  void Finalize(const CertifiedLcsResult &result) const {
    finalizeCertificationAmbiguityDiagnostics(
        result, aTokenCount_, bTokenCount_, diagnosticEvidence_);
  }

private:
  uint64_t aTokenCount_ = 0;
  uint64_t bTokenCount_ = 0;
  LcsCertificationDiagnosticEvidence *diagnosticEvidence_ = nullptr;
};

/// Return the exact left endpoint kind for one local certification rectangle.
static LcsDiagnosticWindowAnchor makeDiagnosticLeftBoundary(
    uint64_t aBoundary, uint64_t bBoundary) {
  if (aBoundary == 0 && bBoundary == 0)
    return LcsDiagnosticWindowAnchor::StreamBegin();
  return LcsDiagnosticWindowAnchor::CertifiedBoundary(aBoundary, bBoundary);
}

/// Return the exact right endpoint kind for one local certification rectangle.
static LcsDiagnosticWindowAnchor makeDiagnosticRightBoundary(
    uint64_t aBoundary, uint64_t bBoundary, uint64_t globalATokenCount,
    uint64_t globalBTokenCount) {
  if (aBoundary == globalATokenCount && bBoundary == globalBTokenCount)
    return LcsDiagnosticWindowAnchor::StreamEnd(globalATokenCount,
                                                globalBTokenCount);
  return LcsDiagnosticWindowAnchor::CertifiedBoundary(aBoundary, bBoundary);
}

/// Append one physical obligation without manufacturing a B-frontier fact.
static void appendIncompleteBoundaryProjection(
    const LcsProtectedBoundaryDiagnosticIdentity &identity,
    LcsAmbiguityWindowDiagnosticRecord &window) {
  LcsProtectedBoundaryDiagnosticProjection projection;
  projection.boundaryIdentityId = identity.identityId;
  projection.aBoundary = identity.aBoundary;
  window.protectedBoundaryProjections.push_back(std::move(projection));
}

/// Publish one uncertified local rectangle with no pair or frontier facts.
static void appendUncertifiedAmbiguityDiagnostic(
    uint64_t globalATokenCount, uint64_t globalBTokenCount,
    const LcsWindowCertificationResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (!diagnosticEvidence)
    return;

  const LcsCertificationWindow &rectangle = result.window;
  LcsAmbiguityWindowDiagnosticRecord window;
  window.aBegin = rectangle.aBegin;
  window.aEnd = rectangle.aEnd;
  window.bBegin = rectangle.bBegin;
  window.bEnd = rectangle.bEnd;
  window.leftForcedAnchor =
      makeDiagnosticLeftBoundary(rectangle.aBegin, rectangle.bBegin);
  window.rightForcedAnchor = makeDiagnosticRightBoundary(
      rectangle.aEnd, rectangle.bEnd, globalATokenCount, globalBTokenCount);
  window.objective = result.objective;
  window.certificationStatus = rectangle.status;

  for (const LcsProtectedBoundaryDiagnosticIdentity &identity :
       diagnosticEvidence->protectedBoundaryIdentities) {
    if (identity.aBoundary &&
        (*identity.aBoundary < rectangle.aBegin ||
         *identity.aBoundary > rectangle.aEnd))
      continue;
    appendIncompleteBoundaryProjection(identity, window);
  }
  diagnosticEvidence->ambiguityWindows.push_back(std::move(window));
}

/// Exact B-frontier facts for one unique protected A coordinate.
struct LocalProtectedBoundaryFacts {
  uint64_t aBoundary = 0;
  std::vector<uint64_t> admissibleBFrontiers;
  bool projectionComplete = false;
};

static bool localBoundaryFactsPrecede(
    const LocalProtectedBoundaryFacts &candidate, uint64_t aBoundary) {
  return candidate.aBoundary < aBoundary;
}

/// Return the facts for one sorted unique A boundary.
static const LocalProtectedBoundaryFacts *findLocalBoundaryFacts(
    ArrayRef<LocalProtectedBoundaryFacts> facts, uint64_t aBoundary) {
  const auto position = std::lower_bound(
      facts.begin(), facts.end(), aBoundary, localBoundaryFactsPrecede);
  if (position == facts.end() || position->aBoundary != aBoundary)
    return nullptr;
  return &*position;
}

/// Capture exact ambiguity evidence from the already-live local theorem.
///
/// Forced token edges divide the certified rectangle into conditioned
/// subwindows. Every optimal path crosses those edge endpoints and any
/// enclosing certified seams, so the rectangle's forward/suffix tables are an
/// equivalent exact view for each subwindow: pair admissibility, objectives,
/// and protected-boundary frontiers require no second dynamic program.
static void appendCertifiedAmbiguityDiagnostics(
    ArrayRef<StringRef> aWindow, ArrayRef<StringRef> bWindow,
    uint64_t absoluteABegin, uint64_t absoluteBBegin,
    uint64_t globalATokenCount, uint64_t globalBTokenCount, size_t stride,
    const ObjectiveTable &forward, const ObjectiveTable &suffix,
    const LcsObjective &total, ArrayRef<uint8_t> pairFacts,
    const LcsWindowCertificationResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (!diagnosticEvidence)
    return;

  const uint64_t absoluteAEnd = absoluteABegin + aWindow.size();
  const uint64_t absoluteBEnd = absoluteBBegin + bWindow.size();
  std::vector<LcsDiagnosticWindowAnchor> anchors;
  anchors.reserve(result.forcedMap.size() + 2);
  anchors.push_back(
      makeDiagnosticLeftBoundary(absoluteABegin, absoluteBBegin));
  for (size_t localA = 0; localA < result.forcedMap.size(); ++localA) {
    const int64_t absoluteB = result.forcedMap[localA];
    if (absoluteB < 0)
      continue;
    anchors.push_back(LcsDiagnosticWindowAnchor::ForcedToken(
        absoluteABegin + localA, static_cast<uint64_t>(absoluteB)));
  }
  anchors.push_back(makeDiagnosticRightBoundary(
      absoluteAEnd, absoluteBEnd, globalATokenCount, globalBTokenCount));

  for (size_t anchorIndex = 0; anchorIndex + 1 < anchors.size();
       ++anchorIndex) {
    const LcsDiagnosticWindowAnchor &left = anchors[anchorIndex];
    const LcsDiagnosticWindowAnchor &right = anchors[anchorIndex + 1];
    const bool leftIsForced =
        left.kind == LcsDiagnosticWindowAnchorKind::ForcedToken;
    const uint64_t aBegin = left.aToken + (leftIsForced ? 1 : 0);
    const uint64_t bBegin = left.bToken + (leftIsForced ? 1 : 0);
    const uint64_t aEnd = right.aToken;
    const uint64_t bEnd = right.bToken;
    if (aBegin > aEnd || bBegin > bEnd || aBegin < absoluteABegin ||
        bBegin < absoluteBBegin || aEnd > absoluteAEnd ||
        bEnd > absoluteBEnd)
      continue;

    const size_t localABegin = static_cast<size_t>(aBegin - absoluteABegin);
    const size_t localAEnd = static_cast<size_t>(aEnd - absoluteABegin);
    const size_t localBBegin = static_cast<size_t>(bBegin - absoluteBBegin);
    const size_t localBEnd = static_cast<size_t>(bEnd - absoluteBBegin);
    const size_t beginState = localABegin * stride + localBBegin;
    const size_t endState = localAEnd * stride + localBEnd;

    LcsAmbiguityWindowDiagnosticRecord window;
    window.aBegin = aBegin;
    window.aEnd = aEnd;
    window.bBegin = bBegin;
    window.bEnd = bEnd;
    window.leftForcedAnchor = left;
    window.rightForcedAnchor = right;
    window.certificationStatus = LcsWindowCertificationStatus::Certified;
    // The core theorem is already complete before optional evidence is copied.
    // Any later diagnostic limitation may lower only the two evidence flags.
    window.coreCertificationComplete = true;
    window.admissiblePairEnumerationComplete = true;
    window.boundaryProjectionComplete = true;
    if (!subtractObjectivesChecked(total, forward.Get(beginState),
                                   suffix.Get(endState), window.objective)) {
      // A completed forced-anchor theorem guarantees this decomposition. Keep
      // an impossible diagnostic inconsistency observational rather than
      // allowing it to weaken the certified production map.
      window.admissiblePairEnumerationComplete = false;
      window.boundaryProjectionComplete = false;
      diagnosticEvidence->ambiguityWindows.push_back(std::move(window));
      continue;
    }

    size_t optionalPairCount = 0;
    for (size_t localA = localABegin; localA < localAEnd; ++localA) {
      const StringRef spelling = aWindow[localA];
      for (size_t localB = localBBegin; localB < localBEnd; ++localB) {
        if (spelling != bWindow[localB])
          continue;
        const uint8_t facts = pairFacts[localA * bWindow.size() + localB];
        if ((facts & PairOccursOnOptimalPathFact) == 0 ||
            (facts & PairIsForcedFact) != 0)
          continue;
        ++optionalPairCount;
      }
    }
    const bool hasOptionalPair = optionalPairCount != 0;

    std::vector<uint64_t> uniqueABoundaries;
    for (const LcsProtectedBoundaryDiagnosticIdentity &identity :
         diagnosticEvidence->protectedBoundaryIdentities) {
      if (!identity.aBoundary || *identity.aBoundary < aBegin ||
          *identity.aBoundary > aEnd)
        continue;
      uniqueABoundaries.push_back(*identity.aBoundary);
    }
    std::sort(uniqueABoundaries.begin(), uniqueABoundaries.end());
    uniqueABoundaries.erase(
        std::unique(uniqueABoundaries.begin(), uniqueABoundaries.end()),
        uniqueABoundaries.end());

    std::vector<LocalProtectedBoundaryFacts> boundaryFacts;
    boundaryFacts.reserve(uniqueABoundaries.size());
    bool hasAmbiguousProtectedFrontier = false;
    for (uint64_t aBoundary : uniqueABoundaries) {
      LocalProtectedBoundaryFacts facts;
      facts.aBoundary = aBoundary;
      const size_t localA =
          static_cast<size_t>(aBoundary - absoluteABegin);
      for (size_t localB = localBBegin; localB <= localBEnd; ++localB) {
        const size_t state = localA * stride + localB;
        if (objectiveSumEquals(forward.Get(state), suffix.Get(state), total))
          facts.admissibleBFrontiers.push_back(absoluteBBegin + localB);
      }
      facts.projectionComplete = !facts.admissibleBFrontiers.empty();
      hasAmbiguousProtectedFrontier |=
          facts.admissibleBFrontiers.size() != 1;
      boundaryFacts.push_back(std::move(facts));
    }

    if (!hasOptionalPair && !hasAmbiguousProtectedFrontier)
      continue;

    if (hasOptionalPair) {
      uint64_t requiredPairBytes = 0;
      if (!tryChargeAdmissiblePairEvidence(
              optionalPairCount, *diagnosticEvidence, requiredPairBytes)) {
        window.admissiblePairEnumerationComplete = false;
        REFOLD_LOG_TRACE(
            "lcs/diagnostic",
            "admissible-pair evidence omitted: A=[{0},{1}) B=[{2},{3}) "
            "pairs={4} requiredBytes={5} retainedBytes={6} maxBytes={7}",
            aBegin, aEnd, bBegin, bEnd, optionalPairCount, requiredPairBytes,
            diagnosticEvidence->retainedAdmissiblePairBytes,
            diagnosticEvidence->admissiblePairByteBudget);
      } else {
        StringMap<uint32_t> aSpellingCounts;
        StringMap<uint32_t> bSpellingCounts;
        for (size_t localA = localABegin; localA < localAEnd; ++localA)
          ++aSpellingCounts[aWindow[localA]];
        for (size_t localB = localBBegin; localB < localBEnd; ++localB)
          ++bSpellingCounts[bWindow[localB]];

        window.admissiblePairs.reserve(optionalPairCount);
        for (size_t localA = localABegin; localA < localAEnd; ++localA) {
          const StringRef spelling = aWindow[localA];
          for (size_t localB = localBBegin; localB < localBEnd; ++localB) {
            if (spelling != bWindow[localB])
              continue;
            const uint8_t facts =
                pairFacts[localA * bWindow.size() + localB];
            if ((facts & PairOccursOnOptimalPathFact) == 0 ||
                (facts & PairIsForcedFact) != 0)
              continue;
            window.admissiblePairs.push_back(LcsDiagnosticAdmissiblePair{
                absoluteABegin + localA, absoluteBBegin + localB,
                aSpellingCounts.lookup(spelling) > 1,
                bSpellingCounts.lookup(spelling) > 1});
          }
        }
        assert(window.admissiblePairs.size() == optionalPairCount);
      }
    }

    for (const LcsProtectedBoundaryDiagnosticIdentity &identity :
         diagnosticEvidence->protectedBoundaryIdentities) {
      if (!identity.aBoundary) {
        appendIncompleteBoundaryProjection(identity, window);
        window.boundaryProjectionComplete = false;
        continue;
      }
      if (*identity.aBoundary < aBegin || *identity.aBoundary > aEnd)
        continue;

      LcsProtectedBoundaryDiagnosticProjection projection;
      projection.boundaryIdentityId = identity.identityId;
      projection.aBoundary = identity.aBoundary;
      const LocalProtectedBoundaryFacts *facts =
          findLocalBoundaryFacts(boundaryFacts, *identity.aBoundary);
      if (facts) {
        projection.admissibleBFrontiers = facts->admissibleBFrontiers;
        projection.projectionComplete = facts->projectionComplete;
      }
      window.boundaryProjectionComplete &= projection.projectionComplete;
      window.protectedBoundaryProjections.push_back(std::move(projection));
    }
    diagnosticEvidence->ambiguityWindows.push_back(std::move(window));
  }
}

/// Run the existing all-optimal theorem on one local coordinate rectangle.
///
/// `aWindow` and `bWindow` begin at local DP state `(0,0)`. Nonnegative map
/// values are translated by `absoluteBBegin` before publication. When
/// `retainedOracleStorage` is non-null, this call's quadratic state is moved
/// into an oracle instead of being released at return. All ordinary subwindow
/// calls pass null and retain only linear-size maps and proof records.
///
/// The retained storage is always expressed in this rectangle's local
/// coordinates: pair facts, objective tables, and the owner-gap vector are all
/// indexed from `(0,0)`, never from `absoluteABegin`/`absoluteBBegin`. The
/// complete-stream adopter may read those coordinates as absolute only because
/// its rectangle starts at the stream origin; an interior window must be
/// published through `CertifiedLcsResult::windowOracles`, whose contract
/// requires callers to translate.
static bool certifyLcsWindowCore(
    ArrayRef<StringRef> aWindow, ArrayRef<StringRef> bWindow,
    ArrayRef<uint32_t> ownerDepthGap, uint64_t absoluteABegin,
    uint64_t absoluteBBegin, uint64_t globalATokenCount,
    uint64_t globalBTokenCount, unsigned long long maxBytes,
    size_t ownerDepthGapCopyCount, LcsWindowCertificationResult &outResult,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence,
    std::shared_ptr<OptimalTokenAlignmentOracle::Storage>
        *retainedOracleStorage) {
  const size_t n = aWindow.size();
  const size_t m = bWindow.size();
  if (!hasBoundaryVectorSize(n, ownerDepthGap.size()) ||
      absoluteABegin > std::numeric_limits<uint64_t>::max() - n ||
      absoluteBBegin > std::numeric_limits<uint64_t>::max() - m ||
      absoluteBBegin + m > MAX)
    return false;

  const uint64_t absoluteAEnd = absoluteABegin + n;
  const uint64_t absoluteBEnd = absoluteBBegin + m;
  initializeWindowCertificationResult(
      absoluteABegin, absoluteAEnd, absoluteBBegin, absoluteBEnd, maxBytes,
      outResult);
  if (retainedOracleStorage)
    retainedOracleStorage->reset();

  CertifiedLcsAllocationPlan allocationPlan;
  if (!buildCertifiedLcsAllocationPlan(n, m, ownerDepthGapCopyCount,
                                       allocationPlan)) {
    // Checked arithmetic or the compact dominator index could not represent
    // this rectangle. This is a non-budget proof limitation, so do not publish
    // a fabricated byte requirement or misclassify it as BudgetExceeded.
    finishWindowObjectiveLinearSpace(aWindow, bWindow, ownerDepthGap,
                                     outResult);
    appendUncertifiedAmbiguityDiagnostic(globalATokenCount, globalBTokenCount,
                                         outResult, diagnosticEvidence);
    REFOLD_LOG_WARN(
        "lcs/map",
        "all-optimal window certification unavailable: checked allocation "
        "or state index is not representable (A=[{0},{1}), B=[{2},{3}), "
        "aTokens={4}, bTokens={5}, maxBytes={6})",
        absoluteABegin, absoluteAEnd, absoluteBBegin, absoluteBEnd, n, m,
        maxBytes);
    return false;
  }

  outResult.window.requiredBytes = allocationPlan.constructionPeakBytes;
  if (allocationPlan.constructionPeakBytes > maxBytes) {
    outResult.window.status = LcsWindowCertificationStatus::BudgetExceeded;
    finishWindowObjectiveLinearSpace(aWindow, bWindow, ownerDepthGap,
                                     outResult);
    appendUncertifiedAmbiguityDiagnostic(globalATokenCount, globalBTokenCount,
                                         outResult, diagnosticEvidence);
    REFOLD_LOG_WARN(
        "lcs/map",
        "all-optimal window certification unavailable: checked aggregate "
        "allocation exceeds the byte budget (A=[{0},{1}), B=[{2},{3}), "
        "aTokens={4}, bTokens={5}, requiredBytes={6}, maxBytes={7})",
        absoluteABegin, absoluteAEnd, absoluteBBegin, absoluteBEnd, n, m,
        allocationPlan.constructionPeakBytes, maxBytes);
    return false;
  }

  std::shared_ptr<OptimalTokenAlignmentOracle::Storage> storage;
  std::vector<uint8_t> localPairFacts;
  std::vector<uint8_t> *pairFacts = &localPairFacts;
  if (retainedOracleStorage) {
    storage = std::make_shared<OptimalTokenAlignmentOracle::Storage>();
    storage->aTokenCount = n;
    storage->bTokenCount = m;
    storage->maxAllocationBytes = maxBytes;
    storage->retainedAllocationBytes = allocationPlan.retainedBytes;
    storage->ownerDepthGap.assign(ownerDepthGap.begin(), ownerDepthGap.end());
    pairFacts = &storage->pairFacts;
  }

  ObjectiveTable forward;
  ObjectiveTable suffix;
  size_t stride = 0;
  if (!buildCoreLcsDpTables(aWindow, bWindow, ownerDepthGap, forward, suffix,
                            stride)) {
    finishWindowObjectiveLinearSpace(aWindow, bWindow, ownerDepthGap,
                                     outResult);
    appendUncertifiedAmbiguityDiagnostic(globalATokenCount, globalBTokenCount,
                                         outResult, diagnosticEvidence);
    return false;
  }

  auto idx = [&](size_t i, size_t j) -> size_t { return i * stride + j; };
  const LcsObjective total = forward.Get(idx(n, m));
  outResult.objective = total;
  outResult.objectiveIsExact = true;
  pairFacts->assign(allocationPlan.pairCount, 0);

  auto isCoreAdmissible = [&](size_t aToken, size_t bToken) {
    if (aToken >= n || bToken >= m ||
        aWindow[aToken] != bWindow[bToken])
      return false;
    const LcsObjective prefix = forward.Get(idx(aToken, bToken));
    const LcsObjective edge{1, 0};
    const LcsObjective tail = suffix.Get(idx(aToken + 1, bToken + 1));
    return objectiveSumEquals(prefix, edge, tail, total);
  };

  for (size_t aToken = 0; aToken < n; ++aToken) {
    for (size_t bToken = 0; bToken < m; ++bToken) {
      uint8_t &facts = (*pairFacts)[aToken * m + bToken];
      if (aWindow[aToken] != bWindow[bToken])
        continue;
      facts |= TokensEqualFact;
      if (isCoreAdmissible(aToken, bToken))
        facts |= PairOccursOnOptimalPathFact;
    }
  }

  markForcedOptimalPairs(n, m, stride, ownerDepthGap, forward, suffix, total,
                         *pairFacts, absoluteBBegin, outResult.forcedMap);
  outResult.selectedMap = outResult.forcedMap;
  for (size_t aToken = 0; aToken < n; ++aToken) {
    if (outResult.selectedMap[aToken] < 0)
      continue;
    outResult.selectedAnchorProofs[aToken] =
        LcsAnchorProof{LcsAnchorProofKind::CoreOptimalPathForced, 0};
  }

  appendCertifiedAmbiguityDiagnostics(
      aWindow, bWindow, absoluteABegin, absoluteBBegin, globalATokenCount,
      globalBTokenCount, stride, forward, suffix, total, *pairFacts, outResult,
      diagnosticEvidence);

  if (storage) {
    storage->stride = stride;
    storage->globalObjective = total;
    storage->forwardObjectives = std::move(forward);
    storage->suffixObjectives = std::move(suffix);
    *retainedOracleStorage = std::move(storage);
  }

  outResult.window.status = LcsWindowCertificationStatus::Certified;
  return true;
}

static void adoptFullStreamWindowResult(
    LcsWindowCertificationResult &&windowResult,
    std::shared_ptr<OptimalTokenAlignmentOracle::Storage> oracleStorage,
    CertifiedLcsResult &outResult) {
  outResult = CertifiedLcsResult{};
  outResult.forcedMap = std::move(windowResult.forcedMap);
  outResult.selectedMap = std::move(windowResult.selectedMap);
  outResult.selectedAnchorProofs =
      std::move(windowResult.selectedAnchorProofs);
  outResult.globalObjective = windowResult.objective;
  outResult.globalObjectiveIsExact = windowResult.objectiveIsExact;
  outResult.allWindowsCertified = windowResult.window.IsCertified();
  outResult.certificationWindows.push_back(windowResult.window);
  if (oracleStorage)
    outResult.oracle = OptimalTokenAlignmentOracle(std::move(oracleStorage));
}

/// Finish the current one-window fallback without publishing a selected path.
static void finalizeUncertifiedFullStreamResult(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<uint32_t> ownerDepthGap, unsigned long long maxBytes,
    CertifiedLcsResult &result) {
  if (!result.globalObjectiveIsExact) {
    result.globalObjective =
        lcsObjectiveLinearSpaceWeighted(a, b, ownerDepthGap);
    result.globalObjectiveIsExact = true;
  }
  result.allWindowsCertified = false;
  REFOLD_LOG_WARN(
      "lcs/map",
      "all-optimal certification unavailable: suppressing all structured "
      "token anchors (aTokens={0}, bTokens={1}, maxBytes={2})",
      a.size(), b.size(), maxBytes);
}

static bool buildForcedCertifiedResult(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<uint32_t> ownerDepthGap, unsigned long long maxBytes,
    CertifiedLcsResult &outResult,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (!hasBoundaryVectorSize(a.size(), ownerDepthGap.size()))
    return false;

  LcsWindowCertificationResult windowResult;
  std::shared_ptr<OptimalTokenAlignmentOracle::Storage> oracleStorage;
  // The complete-stream compatibility path has two live owner-gap payloads:
  // the caller's derived vector and the oracle's retained copy. Counting both
  // preserves the existing byte threshold exactly while the public subwindow
  // API counts only its one local derived vector.
  const bool certified = certifyLcsWindowCore(
      a, b, ownerDepthGap, /*absoluteABegin=*/0, /*absoluteBBegin=*/0,
      /*globalATokenCount=*/a.size(), /*globalBTokenCount=*/b.size(), maxBytes,
      /*ownerDepthGapCopyCount=*/2, windowResult, diagnosticEvidence,
      &oracleStorage);
  if (windowResult.window.aEnd != a.size() ||
      windowResult.window.bEnd != b.size())
    return false;

  adoptFullStreamWindowResult(std::move(windowResult),
                              std::move(oracleStorage), outResult);
  return certified;
}

/// Preserve the current one-window production contract through the generalized
/// local certifier. A failed proof retains the exact objective but publishes no
/// path-selected anchors.
static CertifiedLcsResult certifyFullStream(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<uint32_t> ownerDepthGap, unsigned long long maxBytes,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  CertifiedLcsResult result;
  if (!buildForcedCertifiedResult(a, b, ownerDepthGap, maxBytes, result,
                                  diagnosticEvidence))
    finalizeUncertifiedFullStreamResult(a, b, ownerDepthGap, maxBytes, result);
  return result;
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

  size_t size() const { return len; }

  uint32_t at(size_t i) const { return base[off + i]; }
};

/// Compute one weighted Hirschberg LCS DP row for the given span views.
static std::vector<Score> computeRowWeighted(const SpanView &aV,
                                             const SpanView &bV,
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

/// Compute exact weighted suffix objectives for every B frontier.
///
/// The returned row stores, at index `j`, the optimum for all of `aV` against
/// `bV[j..m)`. This must be computed directly from right to left: the weighted
/// objective is asymmetric at an A token, because deleting `A[i]` pays gap
/// `i+1`, while inserting B at state `(i,j)` pays gap `i`. Reversing A, B, and
/// the gap vector cannot preserve both transition charges simultaneously.
static std::vector<Score> computeSuffixRowWeighted(const SpanView &aV,
                                                   const SpanView &bV,
                                                   const GapView &gapV) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (gapV.size() != n + 1)
    REFOLD_LOG_FATAL("lcs/map", "internal: gap view length must be A.len+1");

  std::vector<Score> next(m + 1);
  std::vector<Score> current(m + 1);

  // S[n][j]: only B insertions remain, all charged at the terminal A gap.
  next[m] = Score{0, 0};
  for (size_t jj = m; jj > 0; --jj) {
    const size_t j = jj - 1;
    next[j] = next[j + 1];
    next[j].cost += static_cast<uint64_t>(gapV.at(n));
  }

  for (size_t ii = n; ii > 0; --ii) {
    const size_t i = ii - 1;
    current[m] = next[m];
    current[m].cost += static_cast<uint64_t>(gapV.at(i + 1));

    for (size_t jj = m; jj > 0; --jj) {
      const size_t j = jj - 1;
      Score best{0, std::numeric_limits<uint64_t>::max()};

      if (aV.at(i) == bV.at(j)) {
        Score candidate = next[j + 1];
        ++candidate.len;
        best = candidate;
      }

      Score deleteA = next[j];
      deleteA.cost += static_cast<uint64_t>(gapV.at(i + 1));
      if (isCoreBetter(deleteA.len, deleteA.cost, best.len, best.cost))
        best = deleteA;

      Score insertB = current[j + 1];
      insertB.cost += static_cast<uint64_t>(gapV.at(i));
      if (isCoreBetter(insertB.len, insertB.cost, best.len, best.cost))
        best = insertB;

      current[j] = best;
    }
    next.swap(current);
  }
  return next;
}

/// Compute `S[aBoundary][j]` for an ordinary contiguous A/B window.
static std::vector<Score> computeSuffixRowWeighted(
    ArrayRef<StringRef> aWindow, ArrayRef<StringRef> bWindow,
    ArrayRef<uint32_t> ownerDepthGap, size_t aBoundary) {
  if (!hasBoundaryVectorSize(aWindow.size(), ownerDepthGap.size()) ||
      aBoundary > aWindow.size())
    REFOLD_LOG_FATAL("lcs/map",
                     "internal: invalid weighted-LCS suffix row bounds");

  const SpanView aSuffix{aWindow, aBoundary, aWindow.size() - aBoundary,
                         false};
  const SpanView bFull{bWindow, 0, bWindow.size(), false};
  const GapView gapSuffix{ownerDepthGap, aBoundary,
                          aWindow.size() - aBoundary + 1};
  return computeSuffixRowWeighted(aSuffix, bFull, gapSuffix);
}

/// Solve a small weighted-LCS box with the full DP table and append anchors in
/// forward order.
static Score solveSmallWeightedDP(const SpanView &aV, const SpanView &bV,
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

  const Score objective{len(n, m), cost(n, m)};

  // Backtrack (diag, then up, then left)
  size_t i = n;
  size_t j = m;
  while (i > 0 || j > 0) {
    const uint32_t curLen = len(i, j);
    const uint64_t curCost = cost(i, j);
    bool moved = false;

    // Diagonal match
    if (i > 0 && j > 0 && aV.at(i - 1) == bV.at(j - 1)) {
      if (len(i - 1, j - 1) == curLen - 1U && cost(i - 1, j - 1) == curCost) {
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
  return objective;
}

/// Recursively solve weighted LCS with Hirschberg splitting.
///
/// This is the structural-cost counterpart to the unweighted Hirschberg solver.
/// It avoids materializing a full quadratic DP table for large boxes by
/// splitting A in half, choosing the B split that maximizes the weighted core
/// objective, and recursively solving the two induced subproblems. The
/// objective is the same one used by the exact weighted DP path: maximize LCS
/// length, then minimize owner-depth gap cost.
static Score hirschbergWeightedRec(const SpanView &aV, const SpanView &bV,
                                   const GapView &gapV,
                                   std::vector<int64_t> &outMap) {
  const size_t n = aV.size();
  const size_t m = bV.size();
  if (n == 0 || m == 0)
    return Score{};

  // Small exact DP base case.
  const unsigned long long cells = static_cast<unsigned long long>(n + 1ULL) *
                                   static_cast<unsigned long long>(m + 1ULL);
  if (cells <= (1ULL << 20))
    return solveSmallWeightedDP(aV, bV, gapV, outMap);

  // Split A in half. The gap view is split with one extra element on each side
  // because A-side gap costs are indexed at token boundaries, so an A span of
  // length k owns k + 1 gap positions.
  const size_t mid = n / 2;

  const SpanView aLeft{aV.base, aV.off, mid, aV.rev};
  const GapView gapLeft{gapV.base, gapV.off, mid + 1};

  const SpanView aRight{aV.base, aV.off + mid, n - mid, aV.rev};
  const GapView gapRight{gapV.base, gapV.off + mid, (n - mid) + 1};

  // Compute the best weighted LCS objective for every possible B split after
  // solving the left half of A against each prefix of B.
  const std::vector<Score> leftRow = computeRowWeighted(aLeft, bV, gapLeft);

  // Compute the exact objective for the right half of A against every suffix
  // of B. A direct backward recurrence is required because reversing the gap
  // vector does not preserve the asymmetric delete/insert charges.
  const std::vector<Score> rightRow =
      computeSuffixRowWeighted(aRight, bV, gapRight);

  // Choose split j maximizing the core objective (length, then inverse cost).
  // On exact equality, prefer the smallest j for determinism.
  size_t bestJ = 0;
  Score best = leftRow[0] + rightRow[0];
  for (size_t j = 1; j <= m; ++j) {
    Score cand = leftRow[j] + rightRow[j];
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
  return best;
}

struct WeightedLcsSelection {
  std::vector<int64_t> map;
  LcsObjective objective;
};

/// Entry point for the linear-space weighted LCS fallback.
static WeightedLcsSelection
lcsMapABHirschbergWeighted(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                           ArrayRef<uint32_t> ownerDepthGap) {
  WeightedLcsSelection selection;
  selection.map.assign(a.size(), -1);
  const SpanView aV{a, 0, a.size(), false};
  const SpanView bV{b, 0, b.size(), false};
  const GapView gV{ownerDepthGap, 0, ownerDepthGap.size()};
  const Score objective =
      hirschbergWeightedRec(aV, bV, gV, selection.map);
  selection.objective =
      LcsObjective{objective.len, objective.cost};
  return selection;
}

/// Compute only the exact weighted-LCS objective in linear space.
///
/// The structured refolding path uses this helper when the quadratic
/// all-optimal tables exceed the proof budget. Computing the objective remains
/// useful for diagnostics and later resource accounting, but deliberately no
/// concrete alignment is reconstructed: one selected Hirschberg path cannot
/// certify which repeated-token anchors occur on all optimal paths.
static LcsObjective
lcsObjectiveLinearSpaceWeighted(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                                ArrayRef<uint32_t> ownerDepthGap) {
  if (ownerDepthGap.size() != a.size() + 1)
    REFOLD_LOG_FATAL("lcs/map",
                     "internal: ownerDepthGap length must be A.size()+1");

  const SpanView aV{a, 0, a.size(), false};
  const SpanView bV{b, 0, b.size(), false};
  const GapView gV{ownerDepthGap, 0, ownerDepthGap.size()};
  const std::vector<Score> finalRow = computeRowWeighted(aV, bV, gV);
  assert(!finalRow.empty() && "weighted-LCS row always contains B boundary 0");
  const Score &objective = finalRow.back();
  return LcsObjective{objective.len, objective.cost};
}

/// Convert one linear-space row score to the public objective value.
static LcsObjective objectiveFromScore(const Score &score) {
  return LcsObjective{score.len, score.cost};
}

/// Project one local A boundary through the complete weighted objective.
///
/// Every monotone source-to-sink path crosses the requested A row at some B
/// frontier. The exact enclosing objective is therefore the best additive
/// combination of the left-prefix and right-suffix rows. Retaining every
/// frontier that attains that objective proves uniqueness without selecting a
/// Hirschberg split.
static bool projectBoundaryWithOwnerDepth(
    ArrayRef<StringRef> aWindow, ArrayRef<StringRef> bWindow,
    ArrayRef<uint32_t> ownerDepthGap, uint64_t absoluteABegin,
    uint64_t absoluteBBegin, size_t localABoundary,
    LcsBoundaryFrontierProjection &result) {
  result = LcsBoundaryFrontierProjection{};
  if (!hasBoundaryVectorSize(aWindow.size(), ownerDepthGap.size()) ||
      localABoundary > aWindow.size() ||
      absoluteABegin > std::numeric_limits<uint64_t>::max() -
                           localABoundary ||
      absoluteBBegin > std::numeric_limits<uint64_t>::max() -
                           bWindow.size())
    return false;

  result.aBoundary = absoluteABegin + localABoundary;
  const size_t bWidth = bWindow.size();
  const SpanView aLeft{aWindow, 0, localABoundary, false};
  const SpanView bForward{bWindow, 0, bWidth, false};
  const GapView gapLeft{ownerDepthGap, 0, localABoundary + 1};
  const std::vector<Score> forward =
      computeRowWeighted(aLeft, bForward, gapLeft);
  const std::vector<Score> suffix = computeSuffixRowWeighted(
      aWindow, bWindow, ownerDepthGap, localABoundary);

  assert(forward.size() == bWidth + 1 && suffix.size() == bWidth + 1 &&
         "linear-space frontier rows must cover every B boundary");

  bool haveObjective = false;
  LcsObjective best;
  for (size_t localB = 0; localB <= bWidth; ++localB) {
    LcsObjective combined;
    if (!addObjectivesChecked(objectiveFromScore(forward[localB]),
                              objectiveFromScore(suffix[localB]),
                              combined))
      return false;

    if (!haveObjective ||
        isCoreBetter(combined.matchedTokenCount, combined.ownerDepthCost,
                     best.matchedTokenCount, best.ownerDepthCost)) {
      haveObjective = true;
      best = combined;
      result.admissibleBFrontiers.clear();
      result.admissibleBFrontiers.push_back(absoluteBBegin + localB);
      continue;
    }
    if (combined == best)
      result.admissibleBFrontiers.push_back(absoluteBBegin + localB);
  }

  if (!haveObjective)
    return false;
  result.windowObjective = best;
  result.objectiveIsExact = true;
  return true;
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
      dpLocal(i, j) = (aV.at(i) == bV.at(j))
                          ? dpLocal(i + 1, j + 1) + 1U
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

bool getLcsCertificationRequiredBytes(uint64_t aTokenCount,
                                      uint64_t bTokenCount,
                                      bool retainCompleteOracle,
                                      uint64_t &requiredBytes) {
  requiredBytes = 0;
  if (aTokenCount > std::numeric_limits<size_t>::max() ||
      bTokenCount > std::numeric_limits<size_t>::max())
    return false;

  CertifiedLcsAllocationPlan plan;
  const size_t ownerDepthGapCopyCount = retainCompleteOracle ? 2 : 1;
  if (!buildCertifiedLcsAllocationPlan(
          static_cast<size_t>(aTokenCount),
          static_cast<size_t>(bTokenCount), ownerDepthGapCopyCount, plan))
    return false;
  requiredBytes = plan.constructionPeakBytes;
  return true;
}

bool certifyLcsWindow(
    ArrayRef<StringRef> a, uint64_t aBegin, uint64_t aEnd,
    ArrayRef<StringRef> b, uint64_t bBegin, uint64_t bEnd,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    unsigned long long maxBytes, LcsWindowCertificationResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  canonicalizeDiagnosticIdentityRequests(diagnosticEvidence);
  result = LcsWindowCertificationResult{};
  result.window = LcsCertificationWindow{
      aBegin, aEnd, bBegin, bEnd,
      LcsWindowCertificationStatus::PartitionUnresolved,
      /*requiredBytes=*/0, static_cast<uint64_t>(maxBytes)};

  if (!hasBoundaryVectorSize(a.size(), gapProvenance.size()) ||
      aBegin > aEnd ||
      bBegin > bEnd || aEnd > a.size() || bEnd > b.size() || bEnd > MAX) {
    REFOLD_LOG_WARN(
        "lcs/map",
        "all-optimal window certification unavailable: invalid bounds or "
        "A-gap provenance (A=[{0},{1}), B=[{2},{3}), aTokens={4}, "
        "bTokens={5}, aGapCount={6})",
        aBegin, aEnd, bBegin, bEnd, a.size(), b.size(),
        gapProvenance.size());
    return false;
  }

  const size_t absoluteABegin = static_cast<size_t>(aBegin);
  const size_t absoluteBBegin = static_cast<size_t>(bBegin);
  const size_t aWidth = static_cast<size_t>(aEnd - aBegin);
  const size_t bWidth = static_cast<size_t>(bEnd - bBegin);

  // Materialize only the A-gap profile needed by this conditioned window.
  // The resulting k+1 vector is part of the checked local allocation payload;
  // no complete-stream owner vector is copied for an isolated certification.
  std::vector<uint32_t> ownerDepthGap =
      copyOwnerDepthGaps(gapProvenance, absoluteABegin,
                         absoluteABegin + aWidth);

  return certifyLcsWindowCore(
      a.slice(absoluteABegin, aWidth),
      b.slice(absoluteBBegin, bWidth), ownerDepthGap, aBegin, bBegin,
      /*globalATokenCount=*/a.size(), /*globalBTokenCount=*/b.size(), maxBytes,
      /*ownerDepthGapCopyCount=*/1, result, diagnosticEvidence,
      /*retainedOracleStorage=*/nullptr);
}

bool retainCertifiedWindowOracle(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance, unsigned long long maxBytes,
    size_t windowIndex, CertifiedLcsResult &result) {
  // An already-usable oracle, including the historical complete-stream one, is
  // answered without recomputing anything.
  if (result.GetSemanticOracleForWindow(windowIndex))
    return true;

  if (windowIndex >= result.certificationWindows.size() ||
      !hasBoundaryVectorSize(a.size(), gapProvenance.size()) ||
      b.size() > MAX || !result.globalObjectiveIsExact ||
      result.forcedMap.size() != a.size() ||
      result.selectedMap.size() != a.size() ||
      result.selectedAnchorProofs.size() != a.size() ||
      !result.CertificationPartitionIsWellFormed(a.size(), b.size()))
    return false;

  const LcsCertificationWindow window =
      result.certificationWindows[windowIndex];
  if (!window.IsCertified())
    return false;

  const size_t aWidth = static_cast<size_t>(window.aEnd - window.aBegin);
  const size_t bWidth = static_cast<size_t>(window.bEnd - window.bBegin);

  // Retention keeps a second owner-gap payload alive for the lifetime of the
  // oracle, so it is charged the complete-oracle requirement rather than the
  // isolated-window one the original pass checked. Test the affordability
  // through the shared helper first: a window that certified without retention
  // is not automatically affordable with it, and that is a fail-closed outcome
  // rather than a certification failure to warn about.
  uint64_t requiredBytes = 0;
  if (!getLcsCertificationRequiredBytes(aWidth, bWidth,
                                        /*retainCompleteOracle=*/true,
                                        requiredBytes) ||
      requiredBytes > maxBytes) {
    REFOLD_LOG_TRACE(
        "lcs/oracle",
        "window oracle retention declined: window={0} A=[{1},{2}) B=[{3},{4}) "
        "requiredBytes={5} maxBytes={6}",
        windowIndex, window.aBegin, window.aEnd, window.bBegin, window.bEnd,
        requiredBytes, maxBytes);
    return false;
  }

  std::vector<uint32_t> ownerDepthGap = copyOwnerDepthGaps(
      gapProvenance, static_cast<size_t>(window.aBegin),
      static_cast<size_t>(window.aBegin) + aWidth);

  LcsWindowCertificationResult localResult;
  std::shared_ptr<OptimalTokenAlignmentOracle::Storage> oracleStorage;
  if (!certifyLcsWindowCore(
          a.slice(static_cast<size_t>(window.aBegin), aWidth),
          b.slice(static_cast<size_t>(window.bBegin), bWidth), ownerDepthGap,
          window.aBegin, window.bBegin, /*globalATokenCount=*/a.size(),
          /*globalBTokenCount=*/b.size(), maxBytes,
          /*ownerDepthGapCopyCount=*/2, localResult,
          /*diagnosticEvidence=*/nullptr, &oracleStorage) ||
      !oracleStorage || !localResult.window.IsCertified() ||
      localResult.window.aBegin != window.aBegin ||
      localResult.window.aEnd != window.aEnd ||
      localResult.window.bBegin != window.bBegin ||
      localResult.window.bEnd != window.bEnd ||
      localResult.forcedMap.size() != aWidth)
    return false;

  // The recertification is admitted only as agreement with the published
  // proof, never as a revision of it. Reproducing this window's exact forced
  // anchors is what makes the retained pair facts facts about the alignment
  // the planner is actually using; a disagreement means the two passes did not
  // solve the same problem, and no anchor may be read from either.
  for (size_t localA = 0; localA < aWidth; ++localA) {
    if (localResult.forcedMap[localA] !=
        result.forcedMap[static_cast<size_t>(window.aBegin) + localA]) {
      REFOLD_LOG_WARN(
          "lcs/oracle",
          "window oracle retention rejected: recertified forced anchor "
          "disagrees at A[{0}] (retained={1}, published={2})",
          window.aBegin + localA, localResult.forcedMap[localA],
          result.forcedMap[static_cast<size_t>(window.aBegin) + localA]);
      return false;
    }
  }

  if (result.windowOracles.empty())
    result.windowOracles.resize(result.certificationWindows.size());
  else if (result.windowOracles.size() != result.certificationWindows.size())
    return false;
  result.windowOracles[windowIndex] =
      OptimalTokenAlignmentOracle(std::move(oracleStorage));

  // Publish only what the query contract accepts, so a retained oracle that
  // fails any dimension check is dropped here rather than surfacing later.
  if (!result.GetSemanticOracleForWindow(windowIndex)) {
    result.windowOracles[windowIndex] = OptimalTokenAlignmentOracle{};
    return false;
  }
  REFOLD_LOG_TRACE(
      "lcs/oracle",
      "retained window oracle: window={0} A=[{1},{2}) B=[{3},{4}) "
      "requiredBytes={5}",
      windowIndex, window.aBegin, window.aEnd, window.bBegin, window.bEnd,
      requiredBytes);
  return true;
}

bool projectLcsBoundaryToOptimalBFrontiers(
    ArrayRef<StringRef> a, uint64_t aBegin, uint64_t aEnd,
    ArrayRef<StringRef> b, uint64_t bBegin, uint64_t bEnd,
    uint64_t aBoundary, ArrayRef<LcsAGapProvenance> gapProvenance,
    LcsBoundaryFrontierProjection &result) {
  result = LcsBoundaryFrontierProjection{};
  result.aBoundary = aBoundary;
  if (!hasBoundaryVectorSize(a.size(), gapProvenance.size()) ||
      aBegin > aBoundary || aBoundary > aEnd || aEnd > a.size() ||
      bBegin > bEnd || bEnd > b.size() || bEnd > MAX)
    return false;

  const size_t absoluteABegin = static_cast<size_t>(aBegin);
  const size_t absoluteBBegin = static_cast<size_t>(bBegin);
  const size_t aWidth = static_cast<size_t>(aEnd - aBegin);
  const size_t bWidth = static_cast<size_t>(bEnd - bBegin);
  std::vector<uint32_t> ownerDepthGap =
      copyOwnerDepthGaps(gapProvenance, absoluteABegin,
                         absoluteABegin + aWidth);
  return projectBoundaryWithOwnerDepth(
      a.slice(absoluteABegin, aWidth),
      b.slice(absoluteBBegin, bWidth), ownerDepthGap, aBegin, bBegin,
      static_cast<size_t>(aBoundary - aBegin), result);
}

std::vector<uint64_t> nominateLcsPartitionBoundaries(
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<int64_t> forcedMap,
    ArrayRef<uint64_t> additionalABoundaries) {
  std::vector<uint64_t> candidates;
  if (gapProvenance.empty())
    return candidates;

  const size_t aTokenCount = gapProvenance.size() - 1;
  auto addBoundary = [&](uint64_t boundary) {
    if (boundary <= aTokenCount)
      candidates.push_back(boundary);
  };

  // Identity changes across one exact A gap nominate include/file, selected
  // conditional-arm, macro-invocation, and macro-role transitions. Scalar
  // owner depth alone is intentionally insufficient: every interior token in
  // one nested owner may share that depth, so treating depth as a transition
  // would degenerate into testing every token boundary.
  for (size_t boundary = 1; boundary < aTokenCount; ++boundary) {
    const LcsAGapProvenance &profile = gapProvenance[boundary];
    const bool identityChanges =
        profile.leftIncludeId != profile.rightIncludeId ||
        profile.leftCondGroupId != profile.rightCondGroupId ||
        profile.leftCondArmId != profile.rightCondArmId ||
        profile.leftMacroRootId != profile.rightMacroRootId ||
        profile.leftMacroLeafId != profile.rightMacroLeafId ||
        profile.leftMacroRoleMask != profile.rightMacroRoleMask;
    if (identityChanges)
      candidates.push_back(static_cast<uint64_t>(boundary));
  }

  // Consecutive forced token edges already prove every state along the run.
  // Only the two run endpoints are useful as partition nominations; retaining
  // every interior state would add no independence and could turn unchanged
  // stretches back into an all-token candidate set.
  if (!forcedMap.empty()) {
    if (forcedMap.size() != aTokenCount) {
      REFOLD_LOG_WARN(
          "lcs/partition",
          "ignoring forced-anchor boundary nominations with wrong A-token "
          "cardinality (mapSize={0}, aTokens={1})",
          forcedMap.size(), aTokenCount);
    } else {
      size_t aToken = 0;
      while (aToken < forcedMap.size()) {
        if (forcedMap[aToken] < 0) {
          ++aToken;
          continue;
        }
        const size_t runBegin = aToken;
        size_t runEnd = aToken;
        while (runEnd + 1 < forcedMap.size() &&
               forcedMap[runEnd + 1] >= 0 &&
               forcedMap[runEnd] < std::numeric_limits<int64_t>::max() &&
               forcedMap[runEnd + 1] == forcedMap[runEnd] + 1)
          ++runEnd;
        addBoundary(static_cast<uint64_t>(runBegin));
        addBoundary(static_cast<uint64_t>(runEnd + 1));
        aToken = runEnd + 1;
      }
    }
  }

  for (uint64_t boundary : additionalABoundaries)
    addBoundary(boundary);

  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  return candidates;
}

/// Build one exact seam partition, optionally joining an existing run ledger.
///
/// Standalone callers reset generated diagnostic output. Certification drivers
/// pass `false` so candidate/frontier evidence and every later local ambiguity
/// record remain in the same run-scoped ledger.
static bool partitionLcsWindowsLinearSpaceImpl(
    ArrayRef<StringRef> a, uint64_t aBegin, uint64_t aEnd,
    ArrayRef<StringRef> b, uint64_t bBegin, uint64_t bEnd,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<uint64_t> candidateABoundaries,
    LcsWindowPartitionResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence,
    bool resetDiagnosticOutputs) {
  result = LcsWindowPartitionResult{};
  if (resetDiagnosticOutputs)
    resetCertificationDiagnosticOutputs(diagnosticEvidence);
  else
    canonicalizeDiagnosticIdentityRequests(diagnosticEvidence);
  if (!hasBoundaryVectorSize(a.size(), gapProvenance.size()) ||
      aBegin > aEnd || aEnd > a.size() || bBegin > bEnd ||
      bEnd > b.size() || bEnd > MAX)
    return false;

  std::vector<uint64_t> candidates;
  candidates.reserve(candidateABoundaries.size());
  for (uint64_t boundary : candidateABoundaries) {
    if (boundary >= aBegin && boundary <= aEnd)
      candidates.push_back(boundary);
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  if (diagnosticEvidence)
    diagnosticEvidence->candidateABoundaries.assign(candidates.begin(),
                                                     candidates.end());

  const size_t absoluteABegin = static_cast<size_t>(aBegin);
  const size_t absoluteBBegin = static_cast<size_t>(bBegin);
  const size_t aWidth = static_cast<size_t>(aEnd - aBegin);
  const size_t bWidth = static_cast<size_t>(bEnd - bBegin);
  std::vector<uint32_t> ownerDepthGap =
      copyOwnerDepthGaps(gapProvenance, absoluteABegin,
                         absoluteABegin + aWidth);
  const ArrayRef<StringRef> aWindow = a.slice(absoluteABegin, aWidth);
  const ArrayRef<StringRef> bWindow = b.slice(absoluteBBegin, bWidth);

  // An accepted seam lies on every enclosing optimal path. Conditioning on
  // such a seam cannot make another candidate's non-singleton frontier set
  // become singleton: every optimal child path composes with the common
  // optimal remainder. One enclosing-window pass is therefore complete for
  // this candidate set and avoids redundant recursive row computations.
  //
  // The first projection already computes the exact enclosing objective.
  // Adopt that objective instead of running a separate full-row pass before
  // testing candidates. Endpoint nominations remain meaningful because a
  // unique non-endpoint B frontier can isolate leading or trailing insertion.
  for (uint64_t aBoundary : candidates) {
    LcsBoundaryFrontierProjection projection;
    if (!projectBoundaryWithOwnerDepth(
            aWindow, bWindow, ownerDepthGap, aBegin, bBegin,
            static_cast<size_t>(aBoundary - aBegin), projection) ||
        !projection.objectiveIsExact) {
      result = LcsWindowPartitionResult{};
      return false;
    }
    if (!result.objectiveIsExact) {
      result.objective = projection.windowObjective;
      result.objectiveIsExact = true;
    } else if (projection.windowObjective != result.objective) {
      result = LcsWindowPartitionResult{};
      return false;
    }
    if (diagnosticEvidence)
      diagnosticEvidence->boundaryFrontierProjections.push_back(projection);
    if (!projection.ProvesUniqueBoundary())
      continue;
    const uint64_t bBoundary = projection.admissibleBFrontiers.front();
    if ((aBoundary == aBegin && bBoundary == bBegin) ||
        (aBoundary == aEnd && bBoundary == bEnd))
      continue;
    result.certifiedBoundaries.push_back(LcsCertifiedBoundary{
        aBoundary, bBoundary,
        LcsBoundaryProofKind::EveryOptimalPathCrossesState});
  }

  if (!result.objectiveIsExact) {
    result.objective =
        lcsObjectiveLinearSpaceWeighted(aWindow, bWindow, ownerDepthGap);
    result.objectiveIsExact = true;
  }

  uint64_t windowABegin = aBegin;
  uint64_t windowBBegin = bBegin;
  for (const LcsCertifiedBoundary &boundary : result.certifiedBoundaries) {
    const bool makesStateProgress = boundary.aBoundary != windowABegin ||
                                    boundary.bBoundary != windowBBegin;
    if (!boundary.IsAuthorized() || !makesStateProgress ||
        boundary.aBoundary < windowABegin || boundary.aBoundary > aEnd ||
        boundary.bBoundary < windowBBegin || boundary.bBoundary > bEnd) {
      result = LcsWindowPartitionResult{};
      return false;
    }
    result.windows.push_back(LcsCertificationWindow{
        windowABegin, boundary.aBoundary, windowBBegin,
        boundary.bBoundary,
        LcsWindowCertificationStatus::PartitionUnresolved,
        /*requiredBytes=*/0, /*proofBudgetBytes=*/0});
    windowABegin = boundary.aBoundary;
    windowBBegin = boundary.bBoundary;
  }
  result.windows.push_back(LcsCertificationWindow{
      windowABegin, aEnd, windowBBegin, bEnd,
      LcsWindowCertificationStatus::PartitionUnresolved,
      /*requiredBytes=*/0, /*proofBudgetBytes=*/0});

  // Endpoint A seams may isolate leading or trailing insertion-only windows;
  // every emitted child must nevertheless make progress in at least one axis.
  if (result.windows.size() == 1 && aBegin == aEnd && bBegin == bEnd)
    return true;
  for (const LcsCertificationWindow &window : result.windows) {
    if (window.aBegin == window.aEnd && window.bBegin == window.bEnd) {
      result = LcsWindowPartitionResult{};
      return false;
    }
  }
  return true;
}

/// Preserve the established one-window result from provenance input.
///
/// A partition that proves no interior seam is semantically the original
/// complete-stream problem. Reusing the compatibility path avoids changing its
/// retained oracle or byte threshold merely because callers nominated
/// boundaries that were correctly rejected.
static bool certifyFullStreamFromProvenance(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    unsigned long long maxBytes, CertifiedLcsResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  std::vector<uint32_t> ownerDepthGap =
      copyOwnerDepthGaps(gapProvenance, 0, a.size());
  result = certifyFullStream(a, b, ownerDepthGap, maxBytes,
                             diagnosticEvidence);
  return result.globalObjectiveIsExact &&
         result.CertificationPartitionIsWellFormed(a.size(), b.size());
}

/// Merge one local theorem into complete-stream proof surfaces.
///
/// Uncertified windows are required to remain completely suppressed. A
/// certified local map carries absolute B indices, so only its A coordinate
/// needs translation. Every accepted edge is revalidated against the local
/// rectangle, global monotonicity, and its durable core-forced proof before
/// publication.
static bool mergeLocalWindowCertification(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    const LcsCertificationWindow &partitionWindow,
    const LcsWindowCertificationResult &localResult,
    CertifiedLcsResult &result, int64_t &lastPublishedB) {
  const LcsCertificationWindow &window = localResult.window;
  if (window.aBegin != partitionWindow.aBegin ||
      window.aEnd != partitionWindow.aEnd ||
      window.bBegin != partitionWindow.bBegin ||
      window.bEnd != partitionWindow.bEnd)
    return false;

  const size_t aWidth = static_cast<size_t>(window.aEnd - window.aBegin);
  if (!localResult.objectiveIsExact || localResult.forcedMap.size() != aWidth ||
      localResult.selectedMap.size() != aWidth ||
      localResult.selectedAnchorProofs.size() != aWidth)
    return false;

  for (size_t localA = 0; localA < aWidth; ++localA) {
    const int64_t forcedB = localResult.forcedMap[localA];
    const int64_t selectedB = localResult.selectedMap[localA];
    const LcsAnchorProof &proof = localResult.selectedAnchorProofs[localA];
    const size_t absoluteA = static_cast<size_t>(window.aBegin) + localA;

    if (!window.IsCertified()) {
      if (forcedB >= 0 || selectedB >= 0 || proof.IsAuthorized() ||
          proof.semanticWitnessId != 0)
        return false;
      continue;
    }

    if (forcedB < 0 || selectedB < 0) {
      if (forcedB != selectedB || proof.IsAuthorized() ||
          proof.semanticWitnessId != 0)
        return false;
      continue;
    }

    if (forcedB != selectedB || forcedB <= lastPublishedB ||
        proof.kind != LcsAnchorProofKind::CoreOptimalPathForced ||
        proof.semanticWitnessId != 0)
      return false;
    const uint64_t absoluteB = static_cast<uint64_t>(forcedB);
    if (!window.ContainsAnchor(absoluteA, absoluteB) ||
        absoluteA >= a.size() || absoluteB >= b.size() ||
        a[absoluteA] != b[static_cast<size_t>(absoluteB)] ||
        result.forcedMap[absoluteA] >= 0 ||
        result.selectedMap[absoluteA] >= 0 ||
        result.selectedAnchorProofs[absoluteA].IsAuthorized())
      return false;

    result.forcedMap[absoluteA] = forcedB;
    result.selectedMap[absoluteA] = selectedB;
    result.selectedAnchorProofs[absoluteA] = proof;
    lastPublishedB = forcedB;
  }
  return true;
}

/// Certify one already-proved ordered window partition.
///
/// Both the exhaustive diagnostic partitioner and the budget-driven production
/// partitioner terminate here. Keeping publication in one routine prevents the
/// two scheduling policies from drifting in byte accounting, objective
/// composition, or anchor authority.
static bool certifyPartitionWindows(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<LcsCertifiedBoundary> certifiedBoundaries,
    ArrayRef<LcsCertificationWindow> partitionWindows,
    unsigned long long maxBytes, CertifiedLcsResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (partitionWindows.empty() ||
      certifiedBoundaries.size() + 1 != partitionWindows.size())
    return false;

  result = CertifiedLcsResult{};
  result.forcedMap.assign(a.size(), -1);
  result.selectedMap.assign(a.size(), -1);
  result.selectedAnchorProofs.assign(a.size(), LcsAnchorProof{});
  result.globalObjectiveIsExact = true;
  result.allWindowsCertified = true;
  result.certifiedBoundaries.assign(certifiedBoundaries.begin(),
                                    certifiedBoundaries.end());
  result.certificationWindows.clear();
  result.certificationWindows.reserve(partitionWindows.size());

  LcsObjective composedObjective;
  int64_t lastPublishedB = -1;
  for (const LcsCertificationWindow &partitionWindow : partitionWindows) {
    // The local result is deliberately scoped to one iteration. Its quadratic
    // tables, pair facts, and dominator state have already been destroyed when
    // `certifyLcsWindow()` returns; its remaining linear maps are released at
    // the end of this iteration after publication.
    LcsWindowCertificationResult localResult;
    (void)certifyLcsWindow(
        a, partitionWindow.aBegin, partitionWindow.aEnd, b,
        partitionWindow.bBegin, partitionWindow.bEnd, gapProvenance, maxBytes,
        localResult, diagnosticEvidence);
    LcsObjective nextComposedObjective;
    if (!localResult.objectiveIsExact ||
        !addObjectivesChecked(composedObjective, localResult.objective,
                              nextComposedObjective) ||
        !mergeLocalWindowCertification(a, b, partitionWindow, localResult,
                                       result, lastPublishedB)) {
      result = CertifiedLcsResult{};
      return false;
    }
    composedObjective = nextComposedObjective;
    result.allWindowsCertified &= localResult.window.IsCertified();
    result.certificationWindows.push_back(localResult.window);
  }

  result.globalObjective = composedObjective;
  if (!result.CertificationPartitionIsWellFormed(a.size(), b.size())) {
    result = CertifiedLcsResult{};
    return false;
  }
  return true;
}

struct LcsBudgetPartitionPlan {
  SmallVector<LcsCertifiedBoundary, 8> certifiedBoundaries;
  SmallVector<LcsCertificationWindow, 8> windows;
};

/// Insert one evidence-only candidate while retaining normalized order.
static void recordDiagnosticCandidate(
    uint64_t boundary,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (!diagnosticEvidence)
    return;
  auto &candidates = diagnosticEvidence->candidateABoundaries;
  const auto insertion =
      std::lower_bound(candidates.begin(), candidates.end(), boundary);
  if (insertion == candidates.end() || *insertion != boundary)
    candidates.insert(insertion, boundary);
}

/// Return the producer nomination nearest one window's arithmetic midpoint.
///
/// This coordinate is only a fallback proof schedule after the exact midpoint
/// has failed. The selected A coordinate never determines or ranks a B
/// frontier. Ties prefer the smaller A boundary for stable source order.
static std::optional<uint64_t> nearestInteriorCandidate(
    ArrayRef<uint64_t> normalizedCandidates, uint64_t aBegin, uint64_t aEnd,
    uint64_t midpoint) {
  const uint64_t aWidth = aEnd - aBegin;
  const uint64_t quarterWidth = aWidth / 4;
  const uint64_t balancedBegin = aBegin + quarterWidth;
  const uint64_t balancedEnd = aEnd - quarterWidth;
  const auto begin = std::upper_bound(normalizedCandidates.begin(),
                                      normalizedCandidates.end(),
                                      balancedBegin);
  const auto end = std::lower_bound(normalizedCandidates.begin(),
                                    normalizedCandidates.end(), balancedEnd);
  if (begin == end)
    return std::nullopt;

  const auto upper = std::lower_bound(begin, end, midpoint);
  if (upper == begin)
    return *upper;
  if (upper == end)
    return *(end - 1);

  const uint64_t lowerBoundary = *(upper - 1);
  const uint64_t upperBoundary = *upper;
  const uint64_t lowerDistance = midpoint - lowerBoundary;
  const uint64_t upperDistance = upperBoundary - midpoint;
  return lowerDistance <= upperDistance ? lowerBoundary : upperBoundary;
}

/// Recursively split one oversized rectangle at exact balanced state seams.
///
/// At most two boundaries are projected per oversized node: its arithmetic A
/// midpoint and, only if necessary, one producer nomination in the middle half
/// of the A range. An accepted midpoint leaves children with at most half the
/// parent's A width; an accepted producer fallback leaves children with at
/// most three quarters. Thus the sum of child grid areas decreases
/// geometrically regardless of the unique B frontier. This hard work bound is
/// the wall-time property missing from the former one-full-pass-per-candidate
/// implementation.
static bool appendBudgetPartition(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<uint64_t> normalizedCandidates, uint64_t aBegin, uint64_t aEnd,
    uint64_t bBegin, uint64_t bEnd, unsigned long long maxBytes,
    LcsBudgetPartitionPlan &plan,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (aBegin > aEnd || bBegin > bEnd || aEnd > a.size() || bEnd > b.size())
    return false;

  const uint64_t aWidth = aEnd - aBegin;
  const uint64_t bWidth = bEnd - bBegin;
  uint64_t requiredBytes = 0;
  if (getLcsCertificationRequiredBytes(
          aWidth, bWidth, /*retainCompleteOracle=*/false, requiredBytes) &&
      requiredBytes <= maxBytes) {
    plan.windows.push_back(LcsCertificationWindow{
        aBegin, aEnd, bBegin, bEnd,
        LcsWindowCertificationStatus::PartitionUnresolved,
        /*requiredBytes=*/0, /*proofBudgetBytes=*/0});
    return true;
  }

  // There is no interior A row to project. Preserve the exact rectangle as a
  // local fail-closed window; its certifier will record BudgetExceeded or the
  // checked non-representability status and compute its exact objective.
  if (aWidth < 2) {
    plan.windows.push_back(LcsCertificationWindow{
        aBegin, aEnd, bBegin, bEnd,
        LcsWindowCertificationStatus::PartitionUnresolved,
        /*requiredBytes=*/0, /*proofBudgetBytes=*/0});
    return true;
  }

  const uint64_t midpoint = aBegin + aWidth / 2;
  SmallVector<uint64_t, 2> scheduledBoundaries;
  scheduledBoundaries.push_back(midpoint);
  const std::optional<uint64_t> producerBoundary = nearestInteriorCandidate(
      normalizedCandidates, aBegin, aEnd, midpoint);
  if (producerBoundary && *producerBoundary != midpoint)
    scheduledBoundaries.push_back(*producerBoundary);

  for (uint64_t aBoundary : scheduledBoundaries) {
    recordDiagnosticCandidate(aBoundary, diagnosticEvidence);
    LcsBoundaryFrontierProjection projection;
    if (!projectLcsBoundaryToOptimalBFrontiers(
            a, aBegin, aEnd, b, bBegin, bEnd, aBoundary, gapProvenance,
            projection) ||
        !projection.objectiveIsExact)
      return false;
    if (diagnosticEvidence)
      diagnosticEvidence->boundaryFrontierProjections.push_back(projection);
    if (!projection.ProvesUniqueBoundary())
      continue;

    const uint64_t bBoundary = projection.admissibleBFrontiers.front();
    if (bBoundary < bBegin || bBoundary > bEnd)
      return false;

    if (!appendBudgetPartition(a, b, gapProvenance, normalizedCandidates,
                               aBegin, aBoundary, bBegin, bBoundary, maxBytes,
                               plan, diagnosticEvidence))
      return false;
    plan.certifiedBoundaries.push_back(LcsCertifiedBoundary{
        aBoundary, bBoundary,
        LcsBoundaryProofKind::EveryOptimalPathCrossesState});
    return appendBudgetPartition(
        a, b, gapProvenance, normalizedCandidates, aBoundary, aEnd, bBoundary,
        bEnd, maxBytes, plan, diagnosticEvidence);
  }

  // Neither exact projection proved a singleton frontier. Do not search an
  // unbounded number of alternative rows: that was the catastrophic
  // O(candidateCount*N*M) behavior this path is designed to eliminate. The
  // unresolved rectangle remains fail closed and contributes no anchors.
  plan.windows.push_back(LcsCertificationWindow{
      aBegin, aEnd, bBegin, bEnd,
      LcsWindowCertificationStatus::PartitionUnresolved,
      /*requiredBytes=*/0, /*proofBudgetBytes=*/0});
  return true;
}

bool certifyLcsWindowsIndependently(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<uint64_t> candidateABoundaries,
    unsigned long long maxBytes, CertifiedLcsResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  result = CertifiedLcsResult{};
  LcsAmbiguityDiagnosticRun diagnosticRun(a.size(), b.size(),
                                           diagnosticEvidence);
  LcsCertificationDiagnosticEvidence *runEvidence = diagnosticRun.Evidence();
  if (!hasBoundaryVectorSize(a.size(), gapProvenance.size()) ||
      b.size() > MAX)
    return false;

  // Preserve the established one-window path when no partition nominations
  // are supplied. Besides avoiding a redundant linear-space pass, this retains
  // the complete-stream oracle and its exact historical byte accounting.
  if (candidateABoundaries.empty()) {
    if (!certifyFullStreamFromProvenance(a, b, gapProvenance, maxBytes,
                                         result, runEvidence))
      return false;
    diagnosticRun.Finalize(result);
    return true;
  }

  LcsWindowPartitionResult partition;
  if (!partitionLcsWindowsLinearSpaceImpl(
          a, /*aBegin=*/0, static_cast<uint64_t>(a.size()), b,
          /*bBegin=*/0, static_cast<uint64_t>(b.size()), gapProvenance,
          candidateABoundaries, partition, runEvidence,
          /*resetDiagnosticOutputs=*/false) ||
      !partition.objectiveIsExact || partition.windows.empty() ||
      partition.certifiedBoundaries.size() + 1 != partition.windows.size())
    return false;

  if (partition.certifiedBoundaries.empty()) {
    if (!certifyFullStreamFromProvenance(a, b, gapProvenance, maxBytes,
                                         result, runEvidence) ||
        result.globalObjective != partition.objective) {
      result = CertifiedLcsResult{};
      return false;
    }
    diagnosticRun.Finalize(result);
    return true;
  }

  if (!certifyPartitionWindows(
          a, b, gapProvenance, partition.certifiedBoundaries,
          partition.windows, maxBytes, result, runEvidence) ||
      result.globalObjective != partition.objective) {
    result = CertifiedLcsResult{};
    return false;
  }
  diagnosticRun.Finalize(result);
  return true;
}

bool certifyLcsWindowsWithinBudget(
    ArrayRef<StringRef> a, ArrayRef<StringRef> b,
    ArrayRef<LcsAGapProvenance> gapProvenance,
    ArrayRef<uint64_t> candidateABoundaries,
    unsigned long long maxBytes, CertifiedLcsResult &result,
    LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  result = CertifiedLcsResult{};
  LcsAmbiguityDiagnosticRun diagnosticRun(a.size(), b.size(),
                                           diagnosticEvidence);
  LcsCertificationDiagnosticEvidence *runEvidence = diagnosticRun.Evidence();
  if (!hasBoundaryVectorSize(a.size(), gapProvenance.size()) ||
      b.size() > MAX)
    return false;

  std::vector<uint64_t> normalizedCandidates;
  normalizedCandidates.reserve(candidateABoundaries.size());
  for (uint64_t boundary : candidateABoundaries) {
    if (boundary <= a.size())
      normalizedCandidates.push_back(boundary);
  }
  std::sort(normalizedCandidates.begin(), normalizedCandidates.end());
  normalizedCandidates.erase(
      std::unique(normalizedCandidates.begin(), normalizedCandidates.end()),
      normalizedCandidates.end());
  if (runEvidence) {
    runEvidence->candidateABoundaries.assign(
        normalizedCandidates.begin(), normalizedCandidates.end());
  }

  LcsBudgetPartitionPlan plan;
  if (!appendBudgetPartition(
          a, b, gapProvenance, normalizedCandidates, /*aBegin=*/0,
          static_cast<uint64_t>(a.size()), /*bBegin=*/0,
          static_cast<uint64_t>(b.size()), maxBytes, plan,
          runEvidence) ||
      plan.windows.empty() ||
      plan.certifiedBoundaries.size() + 1 != plan.windows.size())
    return false;

  // Preserve the historical complete-stream byte threshold when no exact
  // seam was proved. The complete compatibility theorem retains one extra
  // owner-gap copy for its oracle, and existing budget tests intentionally
  // account for that payload. A genuinely partitioned result uses the smaller
  // local-window accounting because no complete oracle is retained.
  if (plan.certifiedBoundaries.empty() && plan.windows.size() == 1) {
    if (!certifyFullStreamFromProvenance(a, b, gapProvenance, maxBytes,
                                         result, runEvidence))
      return false;
    diagnosticRun.Finalize(result);
    return true;
  }

  if (!certifyPartitionWindows(a, b, gapProvenance,
                               plan.certifiedBoundaries, plan.windows,
                               maxBytes, result, runEvidence))
    return false;
  diagnosticRun.Finalize(result);
  return true;
}

std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<uint32_t> ownerDepthGap,
                              unsigned long long maxBytes) {
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
  const bool useHirschberg = shouldUseLinearSpace(
      n, m, maxBytes, QuadraticLcsAllocationKind::WeightedMap);
  if (useHirschberg) {
    return lcsMapABHirschbergWeighted(a, b, ownerDepthGap).map;
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
      if (len(i - 1, j - 1) == curLen - 1U && cost(i - 1, j - 1) == curCost) {
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

CertifiedLcsResult
certifiedLcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                  ArrayRef<LcsAGapProvenance> gapProvenance,
                  unsigned long long maxBytes,
                  LcsCertificationDiagnosticEvidence *diagnosticEvidence) {
  if (!hasBoundaryVectorSize(a.size(), gapProvenance.size()))
    REFOLD_LOG_FATAL("lcs/map", "gapProvenance length must be A.size() + 1");
  if (b.size() > MAX)
    REFOLD_LOG_FATAL("lcs/map", "B.size() exceeds int64_t index range");

  CertifiedLcsResult result;
  if (!certifyLcsWindowsIndependently(
          a, b, gapProvenance, /*candidateABoundaries=*/{}, maxBytes,
          result, diagnosticEvidence))
    REFOLD_LOG_FATAL("lcs/map", "full-stream LCS certification failed");
  return result;
}

std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              ArrayRef<LcsAGapProvenance> gapProvenance,
                              unsigned long long maxBytes) {
  return certifiedLcsMapAB(a, b, gapProvenance, maxBytes).selectedMap;
}

[[maybe_unused]]
std::vector<int64_t> lcsMapAB(ArrayRef<StringRef> a, ArrayRef<StringRef> b,
                              unsigned long long maxBytes) {
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
  const bool useHirschberg = shouldUseLinearSpace(
      n, m, maxBytes, QuadraticLcsAllocationKind::PlainMap);
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
      dpLocal(i, j) = (a[i] == b[j])
                          ? static_cast<unsigned>(dpLocal(i + 1, j + 1) + 1U)
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

namespace {

/// Append edit hunks from one exact seam-delimited A/B rectangle.
///
/// Token anchors remain ordinary match edges inside the rectangle. The
/// rectangle endpoints are only DP-state conditions, so the trailing edit on
/// the left side and the leading edit on the right side remain distinct even
/// when no matched token occurs at the shared seam.
static bool appendHunksForAlignmentWindow(ArrayRef<int64_t> map,
                                          uint64_t aBegin, uint64_t aEnd,
                                          uint64_t bBegin, uint64_t bEnd,
                                          std::vector<Hunk> &hunks) {
  uint64_t nextA = aBegin;
  uint64_t nextB = bBegin;
  const uint64_t mappedAEnd =
      std::min<uint64_t>(aEnd, static_cast<uint64_t>(map.size()));

  for (uint64_t aToken = aBegin; aToken < mappedAEnd; ++aToken) {
    const int64_t matchedB = map[static_cast<size_t>(aToken)];
    if (matchedB < 0)
      continue;

    const uint64_t bToken = static_cast<uint64_t>(matchedB);
    if (bToken < bBegin || bToken >= bEnd || bToken < nextB)
      return false;

    if (aToken > nextA || bToken > nextB)
      hunks.push_back(Hunk{nextA, aToken, nextB, bToken});

    nextA = aToken + 1;
    nextB = bToken + 1;
  }

  if (nextA < aEnd || nextB < bEnd)
    hunks.push_back(Hunk{nextA, aEnd, nextB, bEnd});
  return true;
}

/// Validate that exact seams form an ordered interior partition.
static bool hunkBoundariesFormMonotonePartition(
    ArrayRef<LcsCertifiedBoundary> certifiedBoundaries, uint64_t limitA,
    uint64_t limitB) {
  uint64_t previousA = 0;
  uint64_t previousB = 0;
  for (const LcsCertifiedBoundary &boundary : certifiedBoundaries) {
    if (!boundary.IsAuthorized() || boundary.aBoundary > limitA ||
        boundary.bBoundary > limitB || boundary.aBoundary < previousA ||
        boundary.bBoundary < previousB ||
        (boundary.aBoundary == previousA &&
         boundary.bBoundary == previousB) ||
        (boundary.aBoundary == limitA && boundary.bBoundary == limitB))
      return false;
    previousA = boundary.aBoundary;
    previousB = boundary.bBoundary;
  }
  return true;
}

} // namespace

std::vector<Hunk>
hunksFromMap(ArrayRef<int64_t> map,
             ArrayRef<LcsCertifiedBoundary> certifiedBoundaries, size_t nA,
             size_t nB) {
  const uint64_t limitA = static_cast<uint64_t>(nA);
  const uint64_t limitB = static_cast<uint64_t>(nB);
  if (!hunkBoundariesFormMonotonePartition(certifiedBoundaries, limitA,
                                            limitB)) {
    REFOLD_LOG_FATAL("lcs/hunks",
                     "certified boundaries do not form a monotone interior "
                     "A/B partition");
  }

  std::vector<Hunk> hunks;
  uint64_t aBegin = 0;
  uint64_t bBegin = 0;
  for (const LcsCertifiedBoundary &boundary : certifiedBoundaries) {
    if (!appendHunksForAlignmentWindow(map, aBegin, boundary.aBoundary,
                                       bBegin, boundary.bBoundary, hunks)) {
      REFOLD_LOG_FATAL("lcs/hunks",
                       "token anchor crosses a certified A/B state seam");
    }
    aBegin = boundary.aBoundary;
    bBegin = boundary.bBoundary;
  }

  if (!appendHunksForAlignmentWindow(map, aBegin, limitA, bBegin, limitB,
                                     hunks)) {
    REFOLD_LOG_FATAL("lcs/hunks",
                     "token anchor lies outside its certified A/B window");
  }
  return hunks;
}

std::vector<Hunk> hunksFromMap(ArrayRef<int64_t> map, size_t nA, size_t nB) {
  return hunksFromMap(map, /*certifiedBoundaries=*/{}, nA, nB);
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
    out.push_back(Step{Op::Equal, aLo + i, aLo + i + 1, bLo + i, bLo + i + 1});
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

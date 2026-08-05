//===--- RefoldSourceMapper.cpp ---------------------------------*- C++ -*-===//
//
// This file implements RefoldSourceMapper, the deterministic A/B source, byte,
// and token coordinate mapping service used by RefoldEngine.  The mapper is
// intentionally limited to source text, token streams, offset tables, and hunk
// caches so coordinate projection does not depend on engine orchestration.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldSourceMapper.h"

#include "core/RefoldLog.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

/// Compute `LCS(a,b[0,j))` for every B-token frontier `j`.
///
/// The row is calculated with linear auxiliary storage. Boundary uniqueness
/// validation uses the complete row rather than one reconstructed LCS path
/// because one deterministic path is not proof that a structural replacement
/// boundary has unique target ownership. Every frontier that participates in
/// any maximum-length local
/// alignment must remain observable until ambiguity is rejected.
std::vector<size_t> buildForwardLCSFrontierRow(ArrayRef<PPTok> a,
                                               ArrayRef<PPTok> b) {
  std::vector<size_t> previous(b.size() + 1, 0);
  std::vector<size_t> current(b.size() + 1, 0);

  for (const PPTok &aToken : a) {
    current[0] = 0;
    for (size_t bIndex = 0; bIndex < b.size(); ++bIndex) {
      if (aToken.spelling == b[bIndex].spelling) {
        current[bIndex + 1] = previous[bIndex] + 1;
      } else {
        current[bIndex + 1] =
            std::max(previous[bIndex + 1], current[bIndex]);
      }
    }
    previous.swap(current);
  }

  return previous;
}

/// Compute `LCS(a,b[j,m))` for every B-token frontier `j`.
///
/// This is the reverse counterpart of `buildForwardLCSFrontierRow()`. For one
/// A split, the sum of the forward and reverse rows at B frontier `j` is the
/// length of the best common subsequence constrained to cross that exact
/// frontier. Consequently all maximum-length local alignments are represented
/// without enumerating or tie-breaking among them.
std::vector<size_t> buildReverseLCSFrontierRow(ArrayRef<PPTok> a,
                                               ArrayRef<PPTok> b) {
  std::vector<size_t> next(b.size() + 1, 0);
  std::vector<size_t> current(b.size() + 1, 0);

  for (size_t aIndex = a.size(); aIndex != 0; --aIndex) {
    current[b.size()] = 0;
    for (size_t bIndex = b.size(); bIndex != 0; --bIndex) {
      const size_t currentB = bIndex - 1;
      if (a[aIndex - 1].spelling == b[currentB].spelling) {
        current[currentB] = next[currentB + 1] + 1;
      } else {
        current[currentB] =
            std::max(next[currentB], current[currentB + 1]);
      }
    }
    next.swap(current);
  }

  return next;
}

} // namespace

StringRef RefoldSourceMapper::SliceSource(ArrayRef<size_t> tokOff,
                                          StringRef source, uint64_t startTok,
                                          uint64_t endTok) {
  if (tokOff.empty() || source.empty())
    return "";

  const size_t n = tokOff.size();
  const uint64_t maxTokIdx = static_cast<uint64_t>(n) - 1;

  // Clamp token indices to valid array bounds.
  uint64_t loTok = std::clamp(startTok, static_cast<uint64_t>(0), maxTokIdx);
  uint64_t hiTok = std::clamp(endTok, loTok, maxTokIdx);

  size_t lo = tokOff[static_cast<size_t>(loTok)];
  size_t hi = tokOff[static_cast<size_t>(hiTok)];

  // Clamp byte offsets to the actual string length.
  const size_t sourceLen = source.size();
  lo = std::clamp(lo, size_t(0), sourceLen);
  hi = std::clamp(hi, lo, sourceLen);

  return source.substr(lo, hi - lo);
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldSourceMapper::BTokenRangeToByteRange(uint64_t bTokBegin,
                                           uint64_t bTokEnd) const {
  // B-token envelopes are half-open, so bTokEnd is allowed to name the
  // sentinel offset immediately after the last B token. Keeping the conversion
  // through bTokOff_ preserves the exact byte spelling chosen by the B lexer.
  if (bTokBegin > bTokEnd || bTokEnd >= bTokOff_.size())
    return std::nullopt;

  const uint64_t begin =
      static_cast<uint64_t>(bTokOff_[static_cast<size_t>(bTokBegin)]);
  const uint64_t end =
      static_cast<uint64_t>(bTokOff_[static_cast<size_t>(bTokEnd)]);
  if (end < begin || end > static_cast<uint64_t>(bSource_.size()))
    return std::nullopt;
  return std::make_pair(begin, end);
}

std::optional<size_t>
RefoldSourceMapper::FindExactOwningArgSpanForPureInsertion(
    uint64_t aPos, ArrayRef<RefoldModel::PPArgSpan> argSpans) const {
  auto isCommaTok = [&](uint64_t a) -> bool {
    return a < aToks_.size() && aToks_[static_cast<size_t>(a)].spelling == ",";
  };

  // First prefer the simple containment case: if the pure-insertion gap lies
  // within an occurrence's half-open A-side span, that occurrence owns it.
  for (size_t i = 0; i < argSpans.size(); ++i) {
    const auto &s = argSpans[i];
    if (aPos >= s.begin && aPos < s.end)
      return i;
  }

  // Next handle the exact separator-before-right-occurrence case. When the gap
  // sits on a comma token, attribute it to an occurrence that begins
  // immediately after that comma.
  if (isCommaTok(aPos)) {
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      if (s.begin == aPos + 1)
        return i;
    }
  }

  // Finally allow an exact span-end owner when no containing occurrence,
  // same-frontier right-hand occurrence, or right-hand separator owner claimed
  // the insertion first. A pure insertion at A token index `aPos` is inserted
  // before that token. Therefore, if an occurrence begins at exactly `aPos`,
  // the insertion is owned by that right-hand half-open span, not by the
  // preceding span whose end is also `aPos`.
  bool hasSameFrontierRightOccurrence = false;
  for (const auto &s : argSpans) {
    if (s.begin == aPos) {
      hasSameFrontierRightOccurrence = true;
      break;
    }
  }

  if (!hasSameFrontierRightOccurrence) {
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      if (aPos == s.end)
        return i;
    }
  }

  // No exact structural owner exists for this pure insertion.
  return std::nullopt;
}

bool RefoldSourceMapper::HunkFullyWithinArgSpans(
    const diffutils::Hunk &h, ArrayRef<RefoldModel::PPArgSpan> argSpans,
    MutableArrayRef<char> touched) const {
  uint64_t a0 = h.aStart;
  uint64_t a1 = h.aEnd;

  auto isCommaTok = [&](uint64_t a) -> bool {
    return a < aToks_.size() && aToks_[static_cast<size_t>(a)].spelling == ",";
  };

  // Pure insertions have no covered A token, so they cannot be attributed by
  // interval containment. Require an exact insertion-site owner instead.
  if (a0 == a1) {
    if (auto owner = FindExactOwningArgSpanForPureInsertion(a0, argSpans)) {
      if (*owner >= touched.size())
        return false;
      touched[*owner] = 1;
      return true;
    }
    return false;
  }

  // For replacement/deletion hunks, every consumed A token must be explainable
  // by an argument-like span. As each token is covered, mark every containing
  // span so later rewrite logic knows which formal/span candidates were
  // touched.
  bool any = false;
  for (uint64_t a = a0; a < a1; ++a) {
    bool inSome = false;

    // Multiple arg-like spans may overlap the same token range. This happens
    // for paste/stringify metadata where several formal-derived surfaces share
    // one PP token but differ by byte subrange. Mark all containing spans
    // rather than choosing one prematurely.
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      if (a >= s.begin && a < s.end) {
        if (i < touched.size())
          touched[i] = 1;
        inSome = true;
        any = true;
      }
    }

    // Permit deletion/replacement of the separator comma immediately before an
    // argument span. This covers cases such as removing a variadic tail where
    // the comma before the first removed variadic argument belongs logically to
    // that right-hand argument group.
    if (!inSome && isCommaTok(a)) {
      for (size_t i = 0; i < argSpans.size(); ++i) {
        const auto &s = argSpans[i];
        if (s.begin == a + 1) {
          if (i < touched.size())
            touched[i] = 1;
          inSome = true;
          any = true;
          break;
        }
      }
    }

    // Any consumed token outside all argument spans means this hunk is not an
    // args-only macro rewrite candidate.
    if (!inSome)
      return false;
  }
  return any;
}

std::vector<diffutils::Hunk>
RefoldSourceMapper::BuildByteHunksFromRawText() const {
  // Diff at byte granularity by representing each source character as a
  // one-character StringRef into the existing source buffers. This avoids
  // copying per-byte strings while still using the shared diff/coalesce path.
  std::vector<StringRef> aRefs = stringutils::splitChars(aSource_);
  std::vector<StringRef> bRefs = stringutils::splitChars(bSource_);

  auto steps = diffutils::diff(aRefs, bRefs);
  return diffutils::coalesce(steps);
}

void RefoldSourceMapper::BuildByteHunkPrefixDeltaCache() {
  abByteHunkPrefixDelta_.clear();

  if (!abByteHunks_ || abByteHunks_->empty())
    return;

  // Prefix deltas let later A-byte -> B-byte projection queries account for the
  // cumulative length drift introduced by all preceding byte hunks.
  abByteHunkPrefixDelta_.reserve(abByteHunks_->size() + 1);
  abByteHunkPrefixDelta_.push_back(0);

  int64_t runningDelta = 0;
  for (const diffutils::Hunk &h : *abByteHunks_) {
    runningDelta += static_cast<int64_t>(h.bEnd - h.bStart) -
                    static_cast<int64_t>(h.aEnd - h.aStart);
    abByteHunkPrefixDelta_.push_back(runningDelta);
  }
}

size_t RefoldSourceMapper::MapAByteToBByteLowerBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // Find the first byte hunk whose A-side range starts at or after `aByte`.
  // Any hunk before this iterator contributes to the cumulative length delta.
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const diffutils::Hunk &h, uint64_t val) { return h.aStart < val; });

  // If `aByte` falls inside the previous edit hunk, it does not have a stable
  // one-to-one byte mapping. Return the start of that hunk's B-side envelope as
  // the lower-bound projection.
  if (it != abByteHunks_->begin()) {
    auto prev = std::prev(it);

    if (searchVal < prev->aEnd) {
      return static_cast<size_t>(prev->bStart);
    }
  }

  // Otherwise `aByte` lies in unchanged text before `it`. Apply the cumulative
  // length drift from all prior hunks to project the byte into B space.
  const size_t prefixIndex =
      static_cast<size_t>(std::distance(abByteHunks_->begin(), it));
  const int64_t delta = prefixIndex < abByteHunkPrefixDelta_.size()
                            ? abByteHunkPrefixDelta_[prefixIndex]
                            : 0;

  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

size_t RefoldSourceMapper::MapAByteToBByteUpperBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // Find the first byte hunk whose A-side range starts at or after `aByte`.
  // Hunks before this point contribute their cumulative length delta directly.
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const diffutils::Hunk &h, uint64_t val) { return h.aStart < val; });

  const size_t prefixIndex =
      static_cast<size_t>(std::distance(abByteHunks_->begin(), it));
  int64_t delta = prefixIndex < abByteHunkPrefixDelta_.size()
                      ? abByteHunkPrefixDelta_[prefixIndex]
                      : 0;

  // Upper-bound projection is inclusive of B-side insertions that occur exactly
  // at the queried A boundary. This differs from the lower-bound mapper, which
  // projects to the beginning of the boundary.
  if (it != abByteHunks_->end()) {
    if (searchVal == it->aStart) {
      if (it->aStart == it->aEnd) {
        // Pure insertion at this A boundary: include the inserted B length in
        // the upper-bound projection.
        delta += static_cast<int64_t>(it->bEnd - it->bStart);
      }
    } else if (searchVal > it->aStart && searchVal < it->aEnd) {
      // Bytes inside a replacement/deletion hunk do not have stable one-to-one
      // mappings. Return the end of the corresponding B-side envelope.
      return static_cast<size_t>(it->bEnd);
    }
  }

  // Project unchanged text by applying the cumulative length drift from all
  // preceding hunks.
  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

std::optional<RefoldSourceMapper::ATokenBoundaryProjection>
RefoldSourceMapper::ProjectATokenBoundaryToBTokenBounds(
    uint64_t parentAStart, uint64_t parentAEnd, uint64_t parentBStart,
    uint64_t parentBEnd, uint64_t aBoundary) const {
  // Boundary uniqueness validation projects only a strict interior boundary
  // of one nonempty replacement. Both A fragments must therefore remain
  // nonempty, and the parent B envelope must itself be well formed and inside
  // the token domains.
  if (parentAStart >= aBoundary || aBoundary >= parentAEnd ||
      parentAEnd > aToks_.size() || parentBStart > parentBEnd ||
      parentBEnd > bToks_.size()) {
    return std::nullopt;
  }

  const ArrayRef<PPTok> parentA =
      aToks_.slice(static_cast<size_t>(parentAStart),
                   static_cast<size_t>(parentAEnd - parentAStart));
  const ArrayRef<PPTok> parentB =
      bToks_.slice(static_cast<size_t>(parentBStart),
                   static_cast<size_t>(parentBEnd - parentBStart));
  const size_t localASplit = static_cast<size_t>(aBoundary - parentAStart);

  // A selected byte or token diff path is insufficient authority here. In a
  // replacement region with repeated spellings, several maximum-length edit
  // alignments can cross the structural A boundary at different B frontiers
  // even though one deterministic reconstruction happens to select only one.
  // Compute the frontier set induced by *all* maximum-length local token
  // alignments instead.
  //
  // For each local B frontier `j`:
  //
  //   prefix[j] = LCS(A[0,split), B[0,j))
  //   suffix[j] = LCS(A[split,n), B[j,m))
  //
  // `prefix[j] + suffix[j]` is the best common subsequence constrained to cross
  // at `j`. Its maximum is the unconstrained local optimum, and exactly those
  // frontiers attaining the maximum occur on at least one maximum-length
  // alignment. The minimum and maximum attaining frontiers are therefore exact
  // lower and upper ownership bounds. Equality proves one boundary;
  // disagreement proves ambiguity and must be preserved for the caller to
  // reject. Owner-depth, line-surface, and preferred-side tie-breaks are
  // intentionally excluded because they can select a placement without token
  // correspondence.
  const std::vector<size_t> prefix = buildForwardLCSFrontierRow(
      parentA.take_front(localASplit), parentB);
  const std::vector<size_t> suffix = buildReverseLCSFrontierRow(
      parentA.drop_front(localASplit), parentB);
  if (prefix.size() != parentB.size() + 1 || suffix.size() != prefix.size())
    return std::nullopt;

  size_t bestScore = 0;
  for (size_t frontier = 0; frontier < prefix.size(); ++frontier)
    bestScore = std::max(bestScore, prefix[frontier] + suffix[frontier]);

  std::optional<size_t> lowerFrontier;
  size_t upperFrontier = 0;
  for (size_t frontier = 0; frontier < prefix.size(); ++frontier) {
    if (prefix[frontier] + suffix[frontier] != bestScore)
      continue;
    if (!lowerFrontier)
      lowerFrontier = frontier;
    upperFrontier = frontier;
  }
  if (!lowerFrontier)
    return std::nullopt;

  ATokenBoundaryProjection projection;
  projection.aBoundary = aBoundary;
  projection.lowerBTokenBoundary =
      parentBStart + static_cast<uint64_t>(*lowerFrontier);
  projection.upperBTokenBoundary =
      parentBStart + static_cast<uint64_t>(upperFrontier);
  return projection;
}

size_t RefoldSourceMapper::BTokIndexFloor(size_t bByte) const {
  if (bTokOff_.size() < 2)
    return 0;

  // `bTokOff_` contains one offset per B token plus a final sentinel at the end
  // of the last token. Therefore the largest real token index is n - 1, and n
  // is the sentinel position used for end-of-stream projections.
  const int n = bTokOff_.size() - 1;

  // Clamp bytes before the first token to the first token index.
  if (bByte <= bTokOff_[0])
    return 0;

  // Clamp bytes at or beyond the sentinel to the sentinel index. Callers can
  // use this as the exclusive end of a B-token envelope.
  if (bByte >= bTokOff_[n])
    return n;

  size_t lo = 0;
  size_t hi = n;

  // Find the greatest token/sentinel offset that is <= `bByte`.
  while (lo < hi) {
    size_t mid = lo + (hi - lo + 1) / 2;
    size_t off = bTokOff_[mid];
    if (off <= bByte)
      lo = mid;
    else
      hi = mid - 1;
  }

  return lo;
}

size_t RefoldSourceMapper::BTokIndexCeil(size_t bByte) const {
  if (bTokOff_.size() < 2)
    return 0;

  // `bTokOff_` contains one offset per B token plus a final sentinel at the end
  // of the last token. Therefore the largest real token index is n - 1, and n
  // is the sentinel position used for end-of-stream projections.
  const int n = bTokOff_.size() - 1;

  // Clamp bytes before the first token to the first token index.
  if (bByte <= bTokOff_[0])
    return 0;

  // Clamp bytes at or beyond the sentinel to the sentinel index. Callers can
  // use this as the exclusive end of a B-token envelope.
  if (bByte >= bTokOff_[n])
    return n;

  size_t lo = 0;
  size_t hi = n;

  // Find the smallest token/sentinel offset that is >= `bByte`.
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t off = bTokOff_[mid];
    if (off < bByte)
      lo = mid + 1;
    else
      hi = mid;
  }

  return lo;
}

std::pair<size_t, size_t>
RefoldSourceMapper::MapAByteRangeToBTokenEnvelope(size_t aByteBegin,
                                                  size_t aByteEnd) const {
  // Normalize malformed ranges to an empty A-byte interval. Callers may pass
  // zero-width ranges for insertion anchors, so do not reject them here.
  if (aByteEnd < aByteBegin)
    aByteEnd = aByteBegin;

  const bool nonEmpty = (aByteEnd > aByteBegin);

  if (inTraceMode()) {
    // Diagnostic aid: boundary pure insertions are the subtle case for envelope
    // mapping. For a non-empty A range, an insertion at exactly the begin/end
    // boundary is not part of the A range's image, but lower/upper byte
    // projection can otherwise make it look adjacent enough to be included.
    auto tracePureInsAt = [&](size_t aByte, llvm::StringRef which) {
      if (!nonEmpty || !abByteHunks_ || abByteHunks_->empty())
        return;

      const uint64_t val = static_cast<uint64_t>(aByte);
      auto it = std::lower_bound(
          abByteHunks_->begin(), abByteHunks_->end(), val,
          [](const diffutils::Hunk &h, uint64_t v) { return h.aStart < v; });

      unsigned shown = 0;
      for (auto cur = it;
           cur != abByteHunks_->end() && cur->aStart == val && cur->aEnd == val;
           ++cur) {
        if (cur->bEnd > cur->bStart) {
          trace("byte/env",
                "A{0} boundary has pure-insertion byte Hunk: A@{1} -> "
                "Bbytes=[{2},{3}) (len={4})",
                which, aByte, static_cast<size_t>(cur->bStart),
                static_cast<size_t>(cur->bEnd),
                static_cast<size_t>(cur->bEnd - cur->bStart));
          if (++shown >= 3)
            break;
        }
      }
    };

    tracePureInsAt(aByteBegin, "Begin");
    tracePureInsAt(aByteEnd, "End");
  }

  // Project the A-byte interval into B-byte space. The lower-bound mapper gives
  // the earliest B byte corresponding to the A start; the upper-bound mapper
  // gives the latest B byte corresponding to the A end.
  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  // For non-empty A ranges, remove pure insertions anchored exactly at the
  // range boundaries from the projected B envelope. This is semantic behavior,
  // not trace-only logging.
  if (nonEmpty && abByteHunks_ && !abByteHunks_->empty()) {
    auto trimBegin = [&]() {
      const uint64_t key = static_cast<uint64_t>(aByteBegin);
      auto it = std::lower_bound(
          abByteHunks_->begin(), abByteHunks_->end(), key,
          [](const diffutils::Hunk &h, uint64_t v) { return h.aStart < v; });

      for (auto cur = it;
           cur != abByteHunks_->end() && cur->aStart == key && cur->aEnd == key;
           ++cur) {
        if (cur->bEnd > cur->bStart) {
          const size_t be = static_cast<size_t>(cur->bEnd);
          if (be > bByteBegin) {
            const size_t oldBegin = bByteBegin;
            bByteBegin = be;
            if (inTraceMode() && bByteBegin > oldBegin) {
              StringRef trimmed = bSource_.slice(
                  oldBegin, std::min(oldBegin + 200, bByteBegin));
              trace("byte/env",
                    "trimBegin: Abytes=[{0},{1}) hunkBbytes=[{2},{3}) "
                    "Btrim=[{4},{5}) text='{6}'",
                    aByteBegin, aByteEnd, cur->bStart, cur->bEnd, oldBegin,
                    bByteBegin, stringutils::showWsWithClip(trimmed, 220));
            }
          }
        }
      }
    };

    auto trimEnd = [&]() {
      const uint64_t key = static_cast<uint64_t>(aByteEnd);
      auto it = std::lower_bound(
          abByteHunks_->begin(), abByteHunks_->end(), key,
          [](const diffutils::Hunk &h, uint64_t v) { return h.aStart < v; });

      for (auto cur = it;
           cur != abByteHunks_->end() && cur->aStart == key && cur->aEnd == key;
           ++cur) {
        if (cur->bEnd > cur->bStart) {
          const size_t bs = static_cast<size_t>(cur->bStart);
          if (bs < bByteEnd) {
            const size_t oldEnd = bByteEnd;
            bByteEnd = bs;
            if (inTraceMode() && oldEnd > bByteEnd) {
              StringRef trimmed =
                  bSource_.slice(bByteEnd, std::min(bByteEnd + 200, oldEnd));
              REFOLD_LOG_TRACE("byte/env",
                               "trimEnd: Abytes=[{0},{1}) hunkBbytes=[{2},{3}) "
                               "Btrim=[{4},{5}) text='{6}'",
                               aByteBegin, aByteEnd, cur->bStart, cur->bEnd,
                               bByteEnd, oldEnd,
                               stringutils::showWsWithClip(trimmed, 220));
            }
          }
        }
      }
    };

    trimBegin();
    trimEnd();
  }

  // Re-normalize after boundary trimming and clamp to the B source buffer.
  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  // Convert the projected B-byte envelope into token indices. The begin uses a
  // floor projection so it includes the token containing the first byte; the
  // end uses a ceiling projection so the returned token range remains
  // exclusive.
  size_t bTokBegin = BTokIndexFloor(bByteBegin);
  size_t bTokEnd = BTokIndexCeil(bByteEnd);

  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return {bTokBegin, bTokEnd};
}

std::pair<size_t, size_t>
RefoldSourceMapper::MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(
    size_t aByteBegin, size_t aByteEnd) const {
  // Normalize malformed ranges to an empty A-byte interval. Unlike the default
  // mapper, this variant intentionally keeps B-side insertions that project to
  // the A-range boundaries.
  if (aByteEnd < aByteBegin)
    aByteEnd = aByteBegin;

  // Project the A-byte range into B-byte space using the normal lower/upper
  // boundary semantics, but do not trim pure insertions anchored at either
  // boundary.
  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  // Convert the B-byte envelope into an exclusive B-token range.
  size_t bTokBegin = BTokIndexFloor(bByteBegin);
  size_t bTokEnd = BTokIndexCeil(bByteEnd);
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return {bTokBegin, bTokEnd};
}

std::optional<std::pair<size_t, size_t>>
RefoldSourceMapper::MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
    uint64_t beginTok, uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());
  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  // Clamp the requested A-token range to the valid token domain. Empty ranges
  // are rejected here because this helper maps a material A-token cover, not a
  // zero-width insertion anchor.
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);
  if (endTok <= beginTok)
    return std::nullopt;

  // Token offsets include a sentinel at the end, so endTok is a valid byte
  // boundary only if it is present in `aTokOff_`.
  size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size())
    return std::nullopt;

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];
  return MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(aByteBegin,
                                                                 aByteEnd);
}

std::optional<std::pair<size_t, size_t>>
RefoldSourceMapper::MapATokRangeToBTokenEnvelopeByTokenDiff(
    uint64_t beginTok, uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);
  if (endTok <= beginTok)
    return std::nullopt;

  // Project an A-token cover into B using the TOKEN-level diff rather than the
  // byte-level diff.  The byte diff can misalign across repeated byte
  // substrings (e.g. the duplicated `int `/` = ` runs produced when a whole
  // statement is inserted ahead of a re-materialized value token), which drifts
  // the projected B envelope into an unrelated boundary insertion.  The token
  // diff is atomic and unambiguous, while still preserving a genuine boundary
  // insertion anchored exactly at the cover's own start/end token.
  const std::vector<diffutils::Hunk> &H = abTokHunks_;

  // Cumulative B-vs-A length delta contributed by every hunk strictly before
  // iterator `it`.
  auto prefixDelta = [&](std::vector<diffutils::Hunk>::const_iterator it) {
    int64_t delta = 0;
    for (auto h = H.begin(); h != it; ++h)
      delta += static_cast<int64_t>(h->bEnd - h->bStart) -
               static_cast<int64_t>(h->aEnd - h->aStart);
    return delta;
  };

  // Lower-bound projection of the cover-begin token.  A token interior to a
  // replace/delete hunk projects to that hunk's B start; otherwise it drifts by
  // the cumulative prefix delta.  A pure boundary insertion anchored exactly at
  // `beginTok` is naturally included: the projection lands on its B start.
  auto lowerBound = [&](uint64_t aTok) -> uint64_t {
    if (H.empty())
      return aTok;
    auto it = std::lower_bound(
        H.begin(), H.end(), aTok,
        [](const diffutils::Hunk &h, uint64_t v) { return h.aStart < v; });
    if (it != H.begin()) {
      auto prev = std::prev(it);
      if (aTok < prev->aEnd)
        return prev->bStart;
    }
    return static_cast<uint64_t>(
        std::max<int64_t>(0, static_cast<int64_t>(aTok) + prefixDelta(it)));
  };

  // Upper-bound projection of the cover-end token.  A boundary insertion whose
  // A anchor equals the end token is included; a token interior to a
  // replace/delete hunk projects to that hunk's B end.
  auto upperBound = [&](uint64_t aTok) -> uint64_t {
    if (H.empty())
      return aTok;
    auto it = std::lower_bound(
        H.begin(), H.end(), aTok,
        [](const diffutils::Hunk &h, uint64_t v) { return h.aStart < v; });
    int64_t delta = prefixDelta(it);
    if (it != H.end()) {
      if (aTok == it->aStart) {
        if (it->aStart == it->aEnd)
          delta += static_cast<int64_t>(it->bEnd - it->bStart);
      } else if (aTok > it->aStart && aTok < it->aEnd) {
        return it->bEnd;
      }
    }
    return static_cast<uint64_t>(
        std::max<int64_t>(0, static_cast<int64_t>(aTok) + delta));
  };

  const uint64_t nB = static_cast<uint64_t>(bToks_.size());
  size_t bLo = static_cast<size_t>(std::min(lowerBound(beginTok), nB));
  size_t bHi = static_cast<size_t>(std::min(upperBound(endTok), nB));
  if (bHi < bLo)
    bHi = bLo;
  if (bHi <= bLo)
    return std::nullopt;
  return std::make_pair(bLo, bHi);
}

std::optional<std::pair<size_t, size_t>>
RefoldSourceMapper::MapAToBTokenEnvelopeByPPArgSpan(
    const RefoldModel::PPArgSpan &sp,
    bool preserveBoundaryInsertions) const {
  // For a standard span that covers exactly one A token, prefer a direct
  // token-index projection when the token survived unchanged. This avoids
  // widening to a byte-derived B envelope that may contain repeated identical
  // spellings after edits, while still failing closed to the general byte
  // mapper if the token was itself edited.
  auto tryMapSingleStandardTokenExactly =
      [&](uint64_t aTokIdx) -> std::optional<std::pair<size_t, size_t>> {
    if (sp.kind != PPArgSpanKind::Standard)
      return std::nullopt;
    if (sp.end != sp.begin + 1)
      return std::nullopt;
    if (static_cast<size_t>(aTokIdx) >= aToks_.size())
      return std::nullopt;

    const StringRef want = aToks_[static_cast<size_t>(aTokIdx)].spelling;
    if (want.empty())
      return std::nullopt;

    // Replay the token-diff length delta up to this A token. If the token falls
    // inside an A-consuming hunk, there is no exact surviving token image to
    // claim, so the caller must use the wider byte-envelope projection instead.
    int64_t delta = 0;
    for (const auto &h : abTokHunks_) {
      if (aTokIdx < h.aStart)
        break;

      if (aTokIdx >= h.aStart && aTokIdx < h.aEnd)
        return std::nullopt;

      delta += static_cast<int64_t>(h.bEnd - h.bStart) -
               static_cast<int64_t>(h.aEnd - h.aStart);
    }

    const int64_t bj = static_cast<int64_t>(aTokIdx) + delta;
    if (bj < 0 || static_cast<size_t>(bj) >= bToks_.size())
      return std::nullopt;
    if (bToks_[static_cast<size_t>(bj)].spelling != want)
      return std::nullopt;

    return std::make_pair(static_cast<size_t>(bj), static_cast<size_t>(bj) + 1);
  };

  // Primary path: use the producer-recorded preprocessed byte span. This is the
  // most precise source of truth for projecting an arg-like A surface into the
  // edited B token stream.
  if (sp.ppByteBegin && sp.ppByteEnd) {
    if (auto exactTokEnv = tryMapSingleStandardTokenExactly(sp.begin)) {
      REFOLD_LOG_TRACE(
          "byte/env",
          "PPArgSpan['{0}' arg={1} Aidx={2} single-token exact map -> "
          "Btok=[{3},{4}) tok='{5}'",
          sp.kind, sp.argIdx, sp.begin, exactTokEnv->first, exactTokEnv->second,
          bToks_[exactTokEnv->first].spelling);
      return exactTokEnv;
    }

    size_t pp0 = static_cast<size_t>(*sp.ppByteBegin);
    size_t pp1 = static_cast<size_t>(*sp.ppByteEnd);
    auto env =
        preserveBoundaryInsertions
            ? MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(pp0, pp1)
            : MapAByteRangeToBTokenEnvelope(pp0, pp1);
    REFOLD_LOG_TRACE("byte/env",
                     "PPArgSpan['{0}' arg={1} Aidx={2} PPbytes=[{3},{4})] "
                     "preserveBoundaryInsertions={5} -> Btok=[{6},{7})",
                     sp.kind, sp.argIdx, sp.begin, pp0, pp1,
                     preserveBoundaryInsertions, env.first, env.second);
    return env;
  }

  // Without producer byte spans, strict mode cannot prove the projection. Do
  // not silently snap token offsets in strict mode because that would downgrade
  // a producer-backed witness into a consumer-side approximation.
  if (strict_) {
    REFOLD_LOG_FATAL(
        "macro/pparg/span",
        "PPArgSpan missing producer ppByte span (kind='{0}' argIdx={1} "
        "A=[{2},{3}) ppByte=[{4},{5}]) - cannot map without snapping",
        sp.kind, sp.argIdx, sp.begin, sp.end, sp.ppByteBegin, sp.ppByteEnd);
    return std::nullopt;
  }

  // Non-strict fallback.  The producer emits a preprocessed byte span for every
  // non-empty, in-bounds arg-like surface, so a missing pp-byte span here means
  // the span is either zero-length -- a zero-token contribution -- or out of
  // bounds.  These are the only two cases, and they are not equivalent:
  //
  //   * A zero-length span has an *exact* consumer-side anchor.  The A-token
  //     offset table gives the precise preprocessed byte at which the empty
  //     contribution sits, so mapping that zero-width A-byte range to B is not
  //     an approximation of a producer fact -- it is the fact.
  //
  //   * A non-empty span without a producer pp-byte span is out of bounds and
  //     cannot be projected without snapping token offsets across unmodeled
  //     material.  Fail closed rather than manufacturing a byte span, matching
  //     the strict path above.
  if (sp.begin != sp.end) {
    REFOLD_LOG_TRACE(
        "macro/pparg/span",
        "PPArgSpan missing producer ppByte span for a non-empty surface "
        "(kind='{0}' argIdx={1} A=[{2},{3})); failing closed rather than "
        "snapping token offsets",
        sp.kind, sp.argIdx, sp.begin, sp.end);
    return std::nullopt;
  }

  const uint64_t maxATok = static_cast<uint64_t>(aToks_.size());
  const uint64_t aIdx = std::clamp(sp.begin, static_cast<uint64_t>(0), maxATok);
  const size_t aByte = aTokOff_[static_cast<size_t>(aIdx)];
  return MapAByteRangeToBTokenEnvelope(aByte, aByte);
}

bool RefoldSourceMapper::TrimPureBoundaryInsertionsFromTokenEnvelope(
    uint64_t beginTok, uint64_t endTok, std::pair<size_t, size_t> &env) const {
  if (abTokHunks_.empty())
    return false;

  size_t bTokBegin = env.first;
  size_t bTokEnd = env.second;
  bool trimmedTokenBoundaryInsertion = false;

  auto trimTokenInsertionAtBoundary = [&](uint64_t aPos, bool isBegin) {
    for (const diffutils::Hunk &h : abTokHunks_) {
      // Only pure token insertions at this exact A boundary are edge material.
      // Replacements and deletions consume covered A tokens and remain part of
      // the owner envelope.
      if (h.aStart != aPos || h.aEnd != aPos || h.bEnd <= h.bStart)
        continue;

      const size_t hb0 = static_cast<size_t>(h.bStart);
      const size_t hb1 = static_cast<size_t>(h.bEnd);

      // Ignore boundary insertions that the byte projection did not include.
      if (!(bTokBegin < hb1 && bTokEnd > hb0))
        continue;

      if (isBegin) {
        const size_t oldBegin = bTokBegin;
        bTokBegin = std::max(bTokBegin, hb1);
        trimmedTokenBoundaryInsertion |= bTokBegin > oldBegin;
        if (inTraceMode() && bTokBegin > oldBegin) {
          REFOLD_LOG_TRACE("byte/env",
                           "trimBeginTok: A=[{0},{1}) boundary A@{2} "
                           "hunkBtok=[{3},{4}) BtrimTok=[{5},{6}) text='{7}'",
                           beginTok, endTok, aPos, hb0, hb1, oldBegin,
                           bTokBegin,
                           stringutils::showWsWithClip(
                               SliceBSource(oldBegin, bTokBegin), 220));
        }
      } else {
        const size_t oldEnd = bTokEnd;
        bTokEnd = std::min(bTokEnd, hb0);
        trimmedTokenBoundaryInsertion |= oldEnd > bTokEnd;
        if (inTraceMode() && oldEnd > bTokEnd) {
          REFOLD_LOG_TRACE(
              "byte/env",
              "trimEndTok: A=[{0},{1}) boundary A@{2} "
              "hunkBtok=[{3},{4}) BtrimTok=[{5},{6}) text='{7}'",
              beginTok, endTok, aPos, hb0, hb1, bTokEnd, oldEnd,
              stringutils::showWsWithClip(SliceBSource(bTokEnd, oldEnd), 220));
        }
      }
    }
  };

  trimTokenInsertionAtBoundary(beginTok, /*isBegin=*/true);
  trimTokenInsertionAtBoundary(endTok, /*isBegin=*/false);

  if (bTokBegin > bTokEnd)
    bTokBegin = bTokEnd;

  env = std::make_pair(bTokBegin, bTokEnd);
  return trimmedTokenBoundaryInsertion;
}

std::optional<std::pair<size_t, size_t>>
RefoldSourceMapper::TryMapUnchangedATokRangeToExactContiguousBImage(
    uint64_t beginTok, uint64_t endTok) const {
  // The byte mapper can legally collapse a surviving single-token image onto
  // an adjacent insertion boundary when the edit is represented as pure B-token
  // insertions around unchanged A tokens. If token-space trimming removes that
  // adjacent insertion, the normal envelope becomes empty even though every
  // covered A token still has an exact B-token image.
  //
  // Recover only that proof surface: no covered A token may be consumed by a
  // replacement/deletion hunk, no pure insertion may appear strictly inside the
  // requested A cover, every projected B token must have the identical
  // spelling, and the projected B tokens must form one contiguous image. Pure
  // insertions at the requested begin/end boundaries are used only to locate
  // the surviving A-token images; they are not included in the returned
  // envelope, so boundary insertion ownership remains with the ordinary
  // insertion owners.
  if (beginTok >= endTok)
    return std::nullopt;

  size_t firstBTok = 0;
  size_t lastBTok = 0;
  size_t prevBTok = 0;
  bool haveImage = false;

  for (uint64_t aTok = beginTok; aTok < endTok; ++aTok) {
    if (static_cast<size_t>(aTok) >= aToks_.size())
      return std::nullopt;

    int64_t delta = 0;
    for (const diffutils::Hunk &h : abTokHunks_) {
      const int64_t aLen =
          static_cast<int64_t>(h.aEnd) - static_cast<int64_t>(h.aStart);
      const int64_t bLen =
          static_cast<int64_t>(h.bEnd) - static_cast<int64_t>(h.bStart);

      if (h.aStart < h.aEnd) {
        // A-consuming hunks mean this A token no longer has an unchanged token
        // image. Let the ordinary byte-envelope path handle such replacements
        // and deletions instead of manufacturing an unchanged-image proof.
        if (h.aStart <= aTok && aTok < h.aEnd)
          return std::nullopt;
        if (h.aEnd <= aTok)
          delta += bLen - aLen;
        continue;
      }

      if (h.bEnd <= h.bStart)
        continue;

      // A pure insertion strictly inside the requested cover would lie between
      // two recovered token images. Returning the enclosing B range would then
      // include B-only material that is not part of the exact unchanged image,
      // so this narrow recovery proof must fail closed.
      if (beginTok < h.aStart && h.aStart < endTok)
        return std::nullopt;

      // A pure insertion at A@N precedes the surviving image of A token N. Pure
      // insertions at beginTok therefore rebase the first covered token image;
      // pure insertions at endTok do not affect any token inside [begin,end).
      if (h.aStart <= aTok)
        delta += bLen;
    }

    const int64_t mapped = static_cast<int64_t>(aTok) + delta;
    if (mapped < 0 || static_cast<size_t>(mapped) >= bToks_.size())
      return std::nullopt;

    const size_t bTok = static_cast<size_t>(mapped);
    if (aToks_[static_cast<size_t>(aTok)].spelling != bToks_[bTok].spelling)
      return std::nullopt;
    if (haveImage && bTok != prevBTok + 1)
      return std::nullopt;

    if (!haveImage) {
      firstBTok = bTok;
      haveImage = true;
    }
    prevBTok = bTok;
    lastBTok = bTok + 1;
  }

  if (!haveImage)
    return std::nullopt;
  return std::make_pair(firstBTok, lastBTok);
}

std::optional<std::pair<size_t, size_t>>
RefoldSourceMapper::MapATokRangeAToBTokenEnvelope(uint64_t beginTok,
                                                  uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  // Clamp the requested token range to the valid A-token domain. Empty ranges
  // are rejected because this helper maps an existing A-token cover, not a
  // zero-width insertion anchor.
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);
  if (endTok <= beginTok)
    return std::nullopt;

  // Token offsets include a sentinel at the end. `endTok` must therefore be a
  // valid offset-table index so the exclusive byte end of the token range can
  // be recovered.
  size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size())
    return std::nullopt;

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];

  // Use the standard byte-range mapper first.  That helper trims
  // boundary-only insertions in byte space, but byte diffs can split a newly
  // inserted token when the inserted spelling shares characters with the first
  // preserved A token (for example, an inserted `struct` before an A range
  // whose first token starts with `typedef`).  Converting such a partially
  // trimmed byte boundary with BTokIndexFloor would re-admit the whole B-only
  // token into this A-cover envelope.
  //
  // Finish the projection in token space as well: pure B-token insertions
  // anchored exactly at the left or right A-token boundary are not part of the
  // image of this non-empty A-token cover.  They must be discharged by their
  // own insertion owner, or by callers that intentionally request the
  // preserve-boundary variant below.  This keeps include/macro/TU owners from
  // materializing the same B-only edge tokens twice.
  std::pair<size_t, size_t> env =
      MapAByteRangeToBTokenEnvelope(aByteBegin, aByteEnd);

  const bool trimmedBoundaryInsertion =
      TrimPureBoundaryInsertionsFromTokenEnvelope(beginTok, endTok, env);

  if (trimmedBoundaryInsertion && env.first == env.second) {
    if (auto exactEnv =
            TryMapUnchangedATokRangeToExactContiguousBImage(beginTok, endTok)) {
      REFOLD_LOG_TRACE(
          "byte/env",
          "recoverExactTokImage: A=[{0},{1}) Btok=[{2},{3}) text='{4}'",
          beginTok, endTok, exactEnv->first, exactEnv->second,
          stringutils::showWsWithClip(
              SliceBSource(exactEnv->first, exactEnv->second), 220));
      return exactEnv;
    }
  }

  return env;
}

std::optional<std::pair<size_t, size_t>>
RefoldSourceMapper::MapATokRangeAToBTokenEnvelopeWholeCover(
    uint64_t beginTok, uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  // Normalize to a non-empty A-token cover. Whole-cover mapping is for
  // materializing an existing A token interval, not for representing an
  // insertion point.
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);
  if (endTok <= beginTok)
    return std::nullopt;

  // `aTokOff_` must provide the exclusive byte boundary for `endTok`, including
  // the final sentinel when the cover reaches the end of A.
  const size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size())
    return std::nullopt;

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];

  // Whole-cover envelopes intentionally use raw lower/upper byte projections
  // and do not trim boundary insertions. The goal is to materialize the full B
  // envelope corresponding to the covered A range.
  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  // Convert the projected B-byte envelope into an exclusive B-token range.
  size_t bTokBegin = BTokIndexFloor(bByteBegin);
  size_t bTokEnd = BTokIndexCeil(bByteEnd);
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return std::make_pair(bTokBegin, bTokEnd);
}

std::optional<std::pair<size_t, size_t>>
RefoldSourceMapper::MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(
    uint64_t beginTok, uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  // Use the same clamped A-token cover as the standard mapper. The trim pass
  // below is defined relative to these exact begin/end A-gap positions.
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);

  auto envOpt = MapATokRangeAToBTokenEnvelope(beginTok, endTok);
  if (!envOpt)
    return std::nullopt;

  if (abTokHunks_.empty())
    return envOpt;

  size_t bBegin = envOpt->first;
  size_t bEnd = envOpt->second;

  auto trimAt = [&](uint64_t aPos, bool isBegin) {
    for (const auto &h : abTokHunks_) {
      // Only token-level pure insertions can be edge-only material.
      // Replacements and deletions consume A tokens and therefore belong to the
      // mapped cover.
      if (h.aStart != aPos || h.aEnd != aPos)
        continue;
      if (h.bEnd <= h.bStart)
        continue;

      const size_t hb0 = static_cast<size_t>(h.bStart);
      const size_t hb1 = static_cast<size_t>(h.bEnd);

      // Ignore insertion hunks that are outside the computed B envelope.
      if (!(bBegin < hb1 && bEnd > hb0))
        continue;

      if (isBegin) {
        // An insertion at the left A boundary precedes the first covered A
        // token, so trim the envelope start past that B-only payload.
        bBegin = std::max(bBegin, hb1);
      } else {
        // An insertion at the right A boundary follows the last covered A
        // token, so trim the envelope end before that B-only payload.
        bEnd = std::min(bEnd, hb0);
      }
    }
  };

  trimAt(beginTok, /*isBegin=*/true);
  trimAt(endTok, /*isBegin=*/false);

  // If both edge trims cross, collapse to an empty B-token envelope instead of
  // returning an inverted range.
  if (bBegin > bEnd)
    bBegin = bEnd;

  return std::make_pair(bBegin, bEnd);
}

std::optional<uint64_t>
RefoldSourceMapper::ByteStartForPPInFile(StringRef file, uint64_t pp) const {
  auto it = model_.GetTokmapByPP().find(pp);
  if (it != model_.GetTokmapByPP().end() &&
      paths_.PathsEqual(it->second.file, file))
    return it->second.b;
  return std::nullopt;
}

std::optional<uint64_t>
RefoldSourceMapper::ByteEndForPPInFile(StringRef file, uint64_t pp) const {
  auto it = model_.GetTokmapByPP().find(pp);
  if (it != model_.GetTokmapByPP().end() &&
      paths_.PathsEqual(it->second.file, file))
    return it->second.e;
  return std::nullopt;
}

} // namespace refold
} // namespace clang

//===--- RefoldEngine.SourceMapping.inc ----------------------*- C++ -*-===//
//
// `RefoldEngine` members physically split out of `RefoldEngine.cpp`.
//
// This fragment groups methods that translate between source, byte, and token
// coordinate systems, recover token envelopes from edited regions, and answer
// "where does this region live in A/B token space?" for later proof and patch
// construction.
//
// This file is included directly by `RefoldEngine.cpp`, so this is only a
// physical file split. No ownership, access, or behavioral semantics change.
//
//===----------------------------------------------------------------------===//

StringRef RefoldEngine::SliceSource(ArrayRef<size_t> tokOff, StringRef source,
                                    uint64_t startTok, uint64_t endTok) {
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

bool RefoldEngine::HunkFullyWithinArgSpans(
    const diffutils::Hunk &h, ArrayRef<RefoldModel::PPArgSpan> argSpans,
    MutableArrayRef<char> touched) const {
  uint64_t a0 = h.aStart;
  uint64_t a1 = h.aEnd;

  auto isCommaTok = [&](uint64_t a) -> bool {
    return a < aToks_.size() &&
           aToks_[static_cast<size_t>(a)].spelling == ",";
  };

  // Pure insertions are attributed only from exact occurrence structure.
  if (a0 == a1) {
    if (auto owner = FindExactOwningArgSpanForPureInsertion(a0, argSpans)) {
      if (*owner >= touched.size())
        return false;
      touched[*owner] = 1;
      return true;
    }
    return false;
  }

  // Replacement/deletion: every covered token must fall inside some arg span.
  // Additionally, allow a separator comma that immediately precedes an arg span
  // (common when deleting the entire variadic tail, which removes the comma
  // after the last fixed formal) to be attributed to that right-hand span.
  bool any = false;
  for (uint64_t a = a0; a < a1; ++a) {
    bool inSome = false;

    // Note: multiple arg-like spans may overlap the same token range (e.g.
    // paste spans that partition a single PP token by byte subranges). In that
    // case, mark *all* containing spans as touched so later logic can pick the
    // one(s) that actually changed.
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      if (a >= s.begin && a < s.end) {
        if (i < touched.size())
          touched[i] = 1;
        inSome = true;
        any = true;
      }
    }

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

    if (!inSome)
      return false;
  }
  return any;
}

std::vector<RefoldEngine::ByteHunk>
RefoldEngine::BuildByteHunksFromRawText() const {
  // We still need a physical array of "elements" for ArrayRef, but now, each
  // element is just 16 bytes (pointer + length) instead of a 32-byte
  // heap-allocating std::string.
  auto ToRefVec = [](StringRef s) {
    std::vector<StringRef> v;
    v.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      // Point to a 1-character substring within the existing aText/bText. This
      // is O(1) and performs NO heap allocation for the character.
      v.push_back(s.substr(i, 1));
    }
    return v;
  };

  std::vector<StringRef> aRefs = ToRefVec(aSource_);
  std::vector<StringRef> bRefs = ToRefVec(bSource_);

  // diff now takes ArrayRef<StringRef>
  auto steps = diffutils::diff(aRefs, bRefs);
  auto hunks = diffutils::coalesce(steps);

  std::vector<ByteHunk> out;
  out.reserve(hunks.size());
  for (const auto &h : hunks) {
    out.emplace_back(h.aStart, h.aEnd, h.bStart, h.bEnd);
  }
  return out;
}

size_t RefoldEngine::MapAByteToBByteLowerBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // 1. Find the first hunk where h.aStart >= aByte
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const ByteHunk &h, uint64_t val) { return h.aStart < val; });

  // 2. We need to look at the hunk PRIOR to 'it' to see if aByte falls inside
  // it, or if we are in the gap after it.
  if (it != abByteHunks_->begin()) {
    auto prev = std::prev(it);

    // If the byte is within the previous hunk's range [aStart, aEnd)
    if (searchVal < prev->aEnd) {
      // It's inside an edit/deletion; map to the start of the B-side
      // equivalent.
      return static_cast<size_t>(prev->bStart);
    }
  }

  // 3. Handle the Cumulative Delta.
  int64_t delta = 0;
  for (auto current = abByteHunks_->begin(); current != it; ++current) {
    delta += (static_cast<int64_t>(current->bEnd - current->bStart) -
              static_cast<int64_t>(current->aEnd - current->aStart));
  }

  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

size_t RefoldEngine::MapAByteToBByteUpperBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // 1. Binary search finds the first hunk where h.aStart >= aByte
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const ByteHunk &h, uint64_t val) { return h.aStart < val; });

  // 3. Accumulate delta from all hunks preceding 'it'
  int64_t delta = 0;
  for (auto current = abByteHunks_->begin(); current != it; ++current) {
    delta += (static_cast<int64_t>(current->bEnd - current->bStart) -
              static_cast<int64_t>(current->aEnd - current->aStart));
  }

  // 4. Handle boundary conditions at the 'it' position
  if (it != abByteHunks_->end()) {
    // If we land exactly on the start of this hunk
    if (searchVal == it->aStart) {
      if (it->aStart == it->aEnd) {
        // Pure insertion at the boundary: Include its shift in the delta.
        delta += static_cast<int64_t>(it->bEnd - it->bStart);
      }
      // Note: We don't snap here because we are at the start of a range.
    } else if (searchVal > it->aStart && searchVal < it->aEnd) {
      // Byte is inside a deletion/replacement hunk: snap to the end.
      return static_cast<size_t>(it->bEnd);
    }
  }

  // 5. Final translation with bounds safety
  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

size_t RefoldEngine::BTokIndexFloor(size_t bByte) const {
  if (bTokOff_.size() < 2)
    return 0;

  const int n = bTokOff_.size() - 1;

  // If the byte is at or before the start of the first token.
  if (bByte <= bTokOff_[0])
    return 0;

  // If the byte is at or after the end of the last token (the sentinel).
  if (bByte >= bTokOff_[n])
    return n;

  size_t lo = 0;
  size_t hi = n;

  // Standard floor binary search.
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

size_t RefoldEngine::BTokIndexCeil(size_t bByte) const {
  if (bTokOff_.size() < 2)
    return 0;

  const int n = bTokOff_.size() - 1;

  // If the byte is at or before the start of the first token.
  if (bByte <= bTokOff_[0])
    return 0;

  // If the byte is at or after the end of the last token (the sentinel).
  if (bByte >= bTokOff_[n])
    return n;

  size_t lo = 0;
  size_t hi = n;

  // Standard ceiling binary search.
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
RefoldEngine::MapAByteRangeToBTokenEnvelope(size_t aByteBegin,
                                            size_t aByteEnd) const {
  // Sanitize A-byte input range
  if (aByteEnd < aByteBegin)
    aByteEnd = aByteBegin;

  // Instrumentation: for non-empty ranges, detect pure insertions anchored
  // exactly at the begin/end A-byte boundaries. These can make the lower-bound
  // mapping choose a B position *before* the insertion, which in turn can
  // cause B-token envelopes to include boundary insertions (a common root cause
  // of duplicated insertion material when a later macro whole-cover patch also
  // slices B).
  const bool nonEmpty = (aByteEnd > aByteBegin);
  auto tracePureInsAt = [&](size_t aByte, llvm::StringRef which) {
    if (!nonEmpty || !abByteHunks_ || abByteHunks_->empty())
      return;

    const uint64_t val = static_cast<uint64_t>(aByte);
    auto it = std::lower_bound(
        abByteHunks_->begin(), abByteHunks_->end(), val,
        [](const ByteHunk &h, uint64_t v) { return h.aStart < v; });

    unsigned shown = 0;
    for (auto cur = it; cur != abByteHunks_->end() && cur->aStart == val &&
                       cur->aEnd == val;
         ++cur) {
      // Pure insertion (A-length 0, B-length >0).
      if (cur->bEnd > cur->bStart) {
        trace("byte/env",
              "A{0} boundary has pure-insertion ByteHunk: A@{1} -> "
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

  // Convert A-byte span to B-byte span using A→B mapping
  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  // For non-empty A spans, exclude any *pure insertion* ByteHunks that are
  // anchored exactly at the begin/end A-byte boundaries.
  //
  // Rationale:
  //   * A pure insertion has A-length 0, so it is not part of the image of a
  //     non-empty A-byte interval under the A→B mapping.
  //   * If we include these boundary insertions in the envelope, callers that
  //     also materialize the insertion as its own edit can end up duplicating
  //     the inserted bytes (notably in macro whole-cover fallback).
  //
  // We only trim boundary insertions for non-empty spans; for empty spans the
  // insertion itself is the whole point.
  if (nonEmpty && abByteHunks_ && !abByteHunks_->empty()) {
    auto trimBegin = [&]() {
      const uint64_t key = static_cast<uint64_t>(aByteBegin);
      auto it = std::lower_bound(
          abByteHunks_->begin(), abByteHunks_->end(), key,
          [](const ByteHunk &h, uint64_t v) { return h.aStart < v; });
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
                    bByteBegin, stringutils::showWSWithClip(trimmed, 220));
            }
          }
        }
      }
    };

    auto trimEnd = [&]() {
      const uint64_t key = static_cast<uint64_t>(aByteEnd);
      auto it = std::lower_bound(
          abByteHunks_->begin(), abByteHunks_->end(), key,
          [](const ByteHunk &h, uint64_t v) { return h.aStart < v; });
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
              trace("byte/env",
                    "trimEnd: Abytes=[{0},{1}) hunkBbytes=[{2},{3}) "
                    "Btrim=[{4},{5}) text='{6}'",
                    aByteBegin, aByteEnd, cur->bStart, cur->bEnd, bByteEnd,
                    oldEnd, stringutils::showWSWithClip(trimmed, 220));
            }
          }
        }
      }
    };

    trimBegin();
    trimEnd();
  }

  // Clamp B-byte bounds to legal range
  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  // Convert B-byte bounds to B-token index span
  size_t bTokBegin = BTokIndexFloor(bByteBegin); // inclusive
  size_t bTokEnd = BTokIndexCeil(bByteEnd);      // exclusive

  // Clamp B-token bounds
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return {bTokBegin, bTokEnd};
}

std::pair<size_t, size_t>
RefoldEngine::MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(
    size_t aByteBegin, size_t aByteEnd) const {
  if (aByteEnd < aByteBegin)
    aByteEnd = aByteBegin;

  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  size_t bTokBegin = BTokIndexFloor(bByteBegin);
  size_t bTokEnd = BTokIndexCeil(bByteEnd);
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return {bTokBegin, bTokEnd};
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
    uint64_t beginTok, uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());
  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);
  if (endTok <= beginTok)
    return std::nullopt;

  size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size())
    return std::nullopt;

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];
  return MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(aByteBegin,
                                                                 aByteEnd);
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapAToBTokenEnvelopeByPPArgSpan(
    const RefoldModel::PPArgSpan &sp) const {
  // When the producer recorded a standard occurrence as exactly one A-side
  // token, prefer an exact token-space projection over byte-envelope snapping.
  // This avoids ambiguous repeated-token selection after edits such as
  // variadic forwarding, where the byte-mapped B envelope may contain several
  // identical spellings (for example multiple `3` tokens) but the token diff
  // already identifies the unique surviving A->B match in order.
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

    int64_t delta = 0;
    for (const auto &h : abTokHunks_) {
      if (aTokIdx < h.aStart)
        break;

      // If the token lies inside an A-consuming edit hunk, it does not have a
      // proven exact token-preserving image in B. Fall back to the byte-span
      // mapper, which is allowed to return a wider replacement envelope.
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

    return std::make_pair(static_cast<size_t>(bj),
                          static_cast<size_t>(bj) + 1);
  };

  // 1. Primary path: Mapping via Preprocessor Byte Spans
  if (sp.ppByteBegin && sp.ppByteEnd) {
    if (auto exactTokEnv = tryMapSingleStandardTokenExactly(sp.begin)) {
      trace("byte/env",
            "PPArgSpan['{0}' arg={1} Aidx={2} single-token exact map -> "
            "Btok=[{3},{4}) tok='{5}'",
            sp.kind, sp.argIdx, sp.begin, exactTokEnv->first,
            exactTokEnv->second,
            bToks_[exactTokEnv->first].spelling);
      return exactTokEnv;
    }

    size_t pp0 = static_cast<size_t>(*sp.ppByteBegin);
    size_t pp1 = static_cast<size_t>(*sp.ppByteEnd);
    auto env = MapAByteRangeToBTokenEnvelope(pp0, pp1);
    trace("byte/env",
          "PPArgSpan['{0}' arg={1} Aidx={2} PPbytes=[{3},{4})] -> "
          "Btok=[{5},{6})",
          sp.kind, sp.argIdx, sp.begin, pp0, pp1, env.first, env.second);
    return env;
  }

  // 2. Don't resort to "fallback" if in strict mode.
  if (strict_) {
    fatal("macro/pparg/span",
          "PPArgSpan missing producer ppByte span (kind='{0}' argIdx={1} "
          "A=[{2},{3}) ppByte=[4},{5}]) - cannot map without snapping",
          sp.kind, sp.argIdx, sp.begin, sp.end, sp.ppByteBegin, sp.ppByteEnd);
    return std::nullopt;
  }

  // 3. Fallback: use consumer token offsets.
  const uint64_t maxATok = static_cast<uint64_t>(aToks_.size());
  uint64_t a0Idx = std::clamp(sp.begin, static_cast<uint64_t>(0), maxATok);
  uint64_t a1Idx = std::clamp(sp.end, a0Idx, maxATok);

  size_t a0 = aTokOff_[static_cast<size_t>(a0Idx)];
  size_t a1 = aTokOff_[static_cast<size_t>(a1Idx)];

  return MapAByteRangeToBTokenEnvelope(a0, a1);
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelope(uint64_t beginTok,
                                            uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  // If we don't have token data, we can't perform the mapping.
  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  // Standardize the token indices.
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);

  // If the range is empty or inverted, return nullopt.
  if (endTok <= beginTok)
    return std::nullopt;

  // Ensure we don't walk off the end of the offset array.
  size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size()) {
    // If we are missing the sentinel, we can't safely determine the end of the
    // last token.
    return std::nullopt;
  }

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];

  // Primary mapping path.
  auto env = MapAByteRangeToBTokenEnvelope(aByteBegin, aByteEnd);

  return env;
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelopeWholeCover(uint64_t beginTok,
                                                      uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);
  if (endTok <= beginTok)
    return std::nullopt;

  const size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size())
    return std::nullopt;

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];

  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  size_t bTokBegin = BTokIndexFloor(bByteBegin);
  size_t bTokEnd = BTokIndexCeil(bByteEnd);
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return std::make_pair(bTokBegin, bTokEnd);
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(
    uint64_t beginTok, uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  // Mirror the clamping performed by MapATokRangeAToBTokenEnvelope so the edge
  // positions we trim are the exact A-token boundaries used for the mapping.
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
      // Only consider token-level pure insertions anchored at this exact A-gap.
      if (h.aStart != aPos || h.aEnd != aPos)
        continue;
      if (h.bEnd <= h.bStart)
        continue;

      const size_t hb0 = static_cast<size_t>(h.bStart);
      const size_t hb1 = static_cast<size_t>(h.bEnd);

      // Only trim if the computed envelope overlaps the insertion payload.
      if (!(bBegin < hb1 && bEnd > hb0))
        continue;

      if (isBegin) {
        // Drop any insertion payload that lies *before* the first A token.
        bBegin = std::max(bBegin, hb1);
      } else {
        // Drop any insertion payload that lies *after* the last A token.
        bEnd = std::min(bEnd, hb0);
      }
    }
  };

  trimAt(beginTok, /*isBegin=*/true);
  trimAt(endTok, /*isBegin=*/false);

  if (bBegin > bEnd)
    bBegin = bEnd;

  return std::make_pair(bBegin, bEnd);
}


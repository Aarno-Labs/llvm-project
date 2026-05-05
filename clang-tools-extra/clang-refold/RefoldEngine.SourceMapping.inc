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

std::vector<RefoldEngine::ByteHunk>
RefoldEngine::BuildByteHunksFromRawText() const {
  // Diff at byte granularity by representing each source character as a
  // one-character StringRef into the existing source buffers. This avoids
  // copying per-byte strings while still using the shared diff/coalesce path.
  std::vector<StringRef> aRefs = stringutils::splitChars(aSource_);
  std::vector<StringRef> bRefs = stringutils::splitChars(bSource_);

  auto steps = diffutils::diff(aRefs, bRefs);
  auto hunks = diffutils::coalesce(steps);

  // Convert generic diff hunks into the engine's byte-hunk representation.
  std::vector<ByteHunk> out;
  out.reserve(hunks.size());
  for (const auto &h : hunks) {
    out.emplace_back(h.aStart, h.aEnd, h.bStart, h.bEnd);
  }
  return out;
}

void RefoldEngine::BuildByteHunkPrefixDeltaCache() {
  abByteHunkPrefixDelta_.clear();

  if (!abByteHunks_ || abByteHunks_->empty())
    return;

  // Prefix deltas let later A-byte -> B-byte projection queries account for the
  // cumulative length drift introduced by all preceding byte hunks.
  abByteHunkPrefixDelta_.reserve(abByteHunks_->size() + 1);
  abByteHunkPrefixDelta_.push_back(0);

  int64_t runningDelta = 0;
  for (const ByteHunk &h : *abByteHunks_) {
    runningDelta += static_cast<int64_t>(h.bEnd - h.bStart) -
                    static_cast<int64_t>(h.aEnd - h.aStart);
    abByteHunkPrefixDelta_.push_back(runningDelta);
  }
}

size_t RefoldEngine::MapAByteToBByteLowerBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // Find the first byte hunk whose A-side range starts at or after `aByte`.
  // Any hunk before this iterator contributes to the cumulative length delta.
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const ByteHunk &h, uint64_t val) { return h.aStart < val; });

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
  const int64_t delta =
      prefixIndex < abByteHunkPrefixDelta_.size()
          ? abByteHunkPrefixDelta_[prefixIndex]
          : 0;

  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

size_t RefoldEngine::MapAByteToBByteUpperBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // Find the first byte hunk whose A-side range starts at or after `aByte`.
  // Hunks before this point contribute their cumulative length delta directly.
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const ByteHunk &h, uint64_t val) { return h.aStart < val; });

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

size_t RefoldEngine::BTokIndexFloor(size_t bByte) const {
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

size_t RefoldEngine::BTokIndexCeil(size_t bByte) const {
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
RefoldEngine::MapAByteRangeToBTokenEnvelope(size_t aByteBegin,
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
          [](const ByteHunk &h, uint64_t v) { return h.aStart < v; });

      unsigned shown = 0;
      for (auto cur = it;
           cur != abByteHunks_->end() && cur->aStart == val && cur->aEnd == val;
           ++cur) {
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
RefoldEngine::MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(
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
RefoldEngine::MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
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
RefoldEngine::MapAToBTokenEnvelopeByPPArgSpan(
    const RefoldModel::PPArgSpan &sp) const {
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
      trace("byte/env",
            "PPArgSpan['{0}' arg={1} Aidx={2} single-token exact map -> "
            "Btok=[{3},{4}) tok='{5}'",
            sp.kind, sp.argIdx, sp.begin, exactTokEnv->first,
            exactTokEnv->second, bToks_[exactTokEnv->first].spelling);
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

  // Without producer byte spans, strict mode cannot prove the projection. Do
  // not silently snap token offsets in strict mode because that would downgrade
  // a producer-backed witness into a consumer-side approximation.
  if (strict_) {
    fatal("macro/pparg/span",
          "PPArgSpan missing producer ppByte span (kind='{0}' argIdx={1} "
          "A=[{2},{3}) ppByte=[4},{5}]) - cannot map without snapping",
          sp.kind, sp.argIdx, sp.begin, sp.end, sp.ppByteBegin, sp.ppByteEnd);
    return std::nullopt;
  }

  // Non-strict fallback: approximate the missing producer byte span from the
  // consumer's A-token offset table, then reuse the ordinary A-byte -> B-token
  // envelope mapper.
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
  // valid offset-table index so the exclusive byte end of the token range can be
  // recovered.
  size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size())
    return std::nullopt;

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];

  // Use the standard byte-range mapper, which trims boundary-only insertions
  // for non-empty A ranges before converting the projected B bytes to tokens.
  return MapAByteRangeToBTokenEnvelope(aByteBegin, aByteEnd);
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelopeWholeCover(uint64_t beginTok,
                                                      uint64_t endTok) const {
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
RefoldEngine::MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(
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

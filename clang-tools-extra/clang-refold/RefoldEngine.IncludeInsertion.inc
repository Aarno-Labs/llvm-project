//===--- RefoldEngine.IncludeInsertion.inc ------------------*- C++ -*-===//
//
// `RefoldEngine` members physically split out of `RefoldEngine.cpp`.
//
// This fragment groups methods that support include realization and include
// patch materialization. These helpers answer "where can an include-backed
// edit be realized in B, and what concrete insertion bytes should be staged?"
//
// This file is included directly by `RefoldEngine.cpp`, so this is only a
// physical file split. No ownership, access, or behavioral semantics change.
//
//===----------------------------------------------------------------------===//

std::optional<std::pair<size_t, size_t>>
RefoldEngine::ResolveIncludeRealizationBTokenEnvelope(
    uint64_t beginTok, uint64_t endTok,
    IncludeRealizationEvidenceKind *evidenceKind) const {
  if (auto canonical = MapATokRangeAToBTokenEnvelope(beginTok, endTok)) {
    // The strongest in-domain witness is the canonical A-cover -> B-envelope
    // mapping. Record that fact explicitly so the include-realization class
    // predicate can distinguish canonical witnesses from the deterministic
    // consensus-rescue fallback below.
    if (evidenceKind)
      *evidenceKind = IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope;
    return canonical;
  }

  // Step 4A keeps unresolved include envelopes as an explicit domain wall.
  // When the canonical mapper cannot recover the B envelope, consult the
  // existing deterministic rescue projections and accept them only if the
  // usable projections agree on one non-empty B-token envelope.
  auto isUsableEnvelope =
      [&](const std::optional<std::pair<size_t, size_t>> &env) -> bool {
    if (!env || env->second <= env->first)
      return false;
    return !SliceBSource(env->first, env->second).trim().empty();
  };

  const auto wholeCover =
      MapATokRangeAToBTokenEnvelopeWholeCover(beginTok, endTok);
  const auto preserveBoundary =
      MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(beginTok, endTok);
  const auto trimEdge =
      MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(beginTok, endTok);

  std::optional<std::pair<size_t, size_t>> consensus;
  bool sawUsableRescue = false;
  bool conflict = false;

  auto consider = [&](StringRef label,
                      const std::optional<std::pair<size_t, size_t>> &env) {
    if (!isUsableEnvelope(env)) {
      trace("include/mat",
            "include-realization rescue envelope unavailable: label={0} "
            "A-cover=[{1},{2})",
            label, beginTok, endTok);
      return;
    }

    sawUsableRescue = true;
    trace("include/mat",
          "include-realization rescue envelope: label={0} A-cover=[{1},{2}) "
          "Btok=[{3},{4}) text={5}",
          label, beginTok, endTok, env->first, env->second,
          stringutils::showWSWithClip(SliceBSource(env->first, env->second),
                                      120));
    if (!consensus) {
      consensus = *env;
      return;
    }
    if (*consensus != *env)
      conflict = true;
  };

  consider("whole-cover", wholeCover);
  consider("preserve-boundary-insertions", preserveBoundary);
  consider("trim-edge-insertions", trimEdge);

  if (!sawUsableRescue || conflict || !consensus)
    return std::nullopt;

  if (evidenceKind)
    *evidenceKind =
        IncludeRealizationEvidenceKind::ConsensusRescuedBCoverEnvelope;
  trace("include/mat",
        "include-realization rescue envelope consensus: A-cover=[{0},{1}) "
        "Btok=[{2},{3})",
        beginTok, endTok, consensus->first, consensus->second);
  return consensus;
}

RefoldEngine::IncludePatch
RefoldEngine::BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                         const diffutils::Hunk &h) const {
  // Extra debug: show the raw PP hunk slices.
  DebugIncludePatch("pre", inc, h);

  std::string insertBytes;
  const size_t numOffsets = bTokOff_.size();
  const size_t uBStart = static_cast<size_t>(h.bStart);
  const size_t uBEnd = static_cast<size_t>(h.bEnd);

  // Validate hunk bounds against B-token offsets.
  if (uBStart < numOffsets && uBEnd < numOffsets && uBEnd >= uBStart) {
    // Replacement and deletion patches have an original header byte span whose
    // untouched leading/trailing whitespace remains in the header when the edit
    // is applied. Therefore the replacement payload must cover exactly the
    // replacement tokens and must not steal the inter-token whitespace before
    // the next B token. This matters for mixed-owner partitions: if an include
    // segment `10 +` is followed by a macro segment `ID(20)`, the newline /
    // indentation between `+` and `20` belongs to the macro/TU boundary, not
    // to the materialized include body. Absorbing it here would create
    // artificial newline drift inside the header, producing gratuitous blank
    // lines in --no-lines mode and duplicate header-local #line resyncs when
    // line directives are enabled.
    //
    // Pure insertions are different: there is no original source span whose
    // surrounding whitespace can be retained, so keep the full token envelope
    // to preserve the inserted B-side payload.
    StringRef bSlice = h.isInsertOnly()
                           ? sliceTokenEnvelope(bTokOff_, bSource_, h.bStart,
                                                h.bEnd)
                           : sliceExactTokenCoverage(bTokOff_, bToks_,
                                                     bSource_, h.bStart,
                                                     h.bEnd);
    insertBytes = bSlice.str();
  } else {
    // Hardening: Log an error or assert if we get a hunk that points outside
    // our known token universe.
    fatal("include/patch",
          "hunk bounds exceed token offset table for inc #{0}: "
          "B[{1},{2}) requested, but bTokOff only has {3} entries",
          inc.id, uBStart, uBEnd, numOffsets);
  }

  IncludePatch patch{&inc, std::move(insertBytes), h.aStart, h.aEnd, h.bStart,
                     h.bEnd};

  // Pre-materialization include patches are internal staging objects only.
  // Do not stamp a normalized accepted path here; the concrete preserving
  // anchor or realization class is chosen later during materialization and
  // only that restamped result may cross a theorem-facing boundary.
  patch.proofSummary = BuildIncludePatchProofSummary(/*realizedSurface=*/false,
                                                    AcceptedPathKind::Unknown,
                                                    &patch);

  trace("include/patch",
        "built inc #{0} patch A[{1},{2})->B[{3},{4}) len(insertBytes)={5}",
        inc.id, patch.aStart, patch.aEnd, patch.bStart, patch.bEnd,
        patch.insertBytes.size());

  return patch;
}

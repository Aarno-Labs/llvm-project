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
    // Prefer the ordinary A-cover -> B-envelope projection. This is the
    // canonical include-realization witness because it follows the same mapping
    // path used for normal A-token covers and needs no non-canonical consensus
    // proof.
    if (evidenceKind)
      *evidenceKind = IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope;
    return canonical;
  }

  // This is not a fallback branch.  It is a declared include-realization
  // evidence class for the narrow case where the canonical mapper cannot
  // recover an envelope but every usable non-canonical boundary projection
  // proves the same non-empty B-token range. Missing projections are ignored;
  // conflicting usable projections are a domain wall.
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
  bool sawUsableBoundaryStableProjection = false;
  bool conflict = false;

  // Add one boundary-stable projection to the consensus set. Unusable envelopes
  // are diagnostic-only; usable envelopes must all equal the first usable
  // envelope.
  auto consider = [&](StringRef label,
                      const std::optional<std::pair<size_t, size_t>> &env) {
    if (!isUsableEnvelope(env)) {
      return;
    }

    sawUsableBoundaryStableProjection = true;

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

  // Fail closed if no deterministic boundary-stable proof produced material
  // text, or if the boundary-stable projections disagree about the B-side
  // envelope.
  if (!sawUsableBoundaryStableProjection || conflict || !consensus)
    return std::nullopt;

  if (evidenceKind)
    *evidenceKind =
        IncludeRealizationEvidenceKind::BoundaryStableConsensusBCoverEnvelope;
  return consensus;
}

RefoldEngine::IncludePatch
RefoldEngine::BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                         const diffutils::Hunk &h) const {
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
    REFOLD_LOG_FATAL("include/patch",
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


  return patch;
}

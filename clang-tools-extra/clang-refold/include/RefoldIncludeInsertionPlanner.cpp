//===--- RefoldIncludeInsertionPlanner.cpp ----------------------*- C++ -*-===//
//
// Include-owned insertion patch construction for clang-refold.
//
// This file implements the standalone planner that builds include-owned
// staging patches and resolves accepted B-token envelopes for inline include
// realization.  The planner is deliberately independent of include
// materialization orchestration.
//
//===----------------------------------------------------------------------===//

#include "include/RefoldIncludeInsertionPlanner.h"

#include "core/RefoldLog.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldIncludeInsertionPlanner::RefoldIncludeInsertionPlanner(
    StringRef bSource, ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff,
    const RefoldSourceMapper &sourceMapper,
    const RefoldProofLattice &proofLattice)
    : bSource_(bSource), bToks_(bToks), bTokOff_(bTokOff),
      sourceMapper_(sourceMapper), proofLattice_(proofLattice) {}

std::optional<std::pair<size_t, size_t>>
RefoldIncludeInsertionPlanner::ResolveIncludeRealizationBTokenEnvelope(
    uint64_t beginTok, uint64_t endTok,
    IncludeRealizationEvidenceKind *evidenceKind) const {
  if (auto canonical =
          sourceMapper_.MapATokRangeAToBTokenEnvelope(beginTok, endTok)) {
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
    return !sourceMapper_.SliceBSource(env->first, env->second).trim().empty();
  };

  // Two boundary-stable projections establish the consensus.  A third mapper
  // used to be consulted here, but it computed the preserve-boundary projection
  // byte for byte, so it could only ever agree and the consensus counted one
  // projection twice.  The genuine third opinion is added below as a *checker*
  // rather than a contributor -- see the token-diff cross-check after the
  // consensus is fixed.
  const auto preserveBoundary =
      sourceMapper_.MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
          beginTok, endTok);
  const auto trimEdge =
      sourceMapper_.MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(beginTok,
                                                                    endTok);

  std::optional<std::pair<size_t, size_t>> consensus;
  bool sawUsableBoundaryStableProjection = false;
  bool conflict = false;

  // Add one boundary-stable projection to the consensus set. Unusable envelopes
  // are diagnostic-only; usable envelopes must all equal the first usable
  // envelope.
  auto consider = [&](const std::optional<std::pair<size_t, size_t>> &env) {
    if (!isUsableEnvelope(env))
      return;

    sawUsableBoundaryStableProjection = true;

    if (!consensus) {
      consensus = *env;
      return;
    }
    if (*consensus != *env)
      conflict = true;
  };

  consider(preserveBoundary);
  consider(trimEdge);

  // Cross-check the fixed consensus against a projection computed on a
  // different basis.  `preserveBoundary` and `trimEdge` both project through
  // the BYTE diff; `MapATokRangeToBTokenEnvelopeByTokenDiff` projects through
  // the TOKEN diff, which its own documentation notes cannot misalign across
  // repeated byte substrings the way the byte diff can.  A disagreement between
  // the two bases means the B envelope is not determined, so it is a domain
  // wall.
  //
  // Deliberately a checker and not a `consider()` participant: this projection
  // may only *reject* a consensus the boundary-stable projections already
  // established, never establish one itself.  Feeding it into the consensus
  // would let it supply an envelope in the case where both boundary-stable
  // projections are unusable, converting a fail-closed refusal into an
  // acceptance -- the opposite of what this gate is for.  An unusable
  // token-diff envelope carries no opinion and is ignored, exactly as an
  // unusable boundary-stable projection is.
  if (consensus) {
    const auto byTokenDiff =
        sourceMapper_.MapATokRangeToBTokenEnvelopeByTokenDiff(beginTok, endTok);
    if (isUsableEnvelope(byTokenDiff) && *byTokenDiff != *consensus)
      conflict = true;
  }

  // Fail closed if no deterministic boundary-stable proof produced material
  // text, or if any usable projection disagrees about the B-side envelope.
  if (!sawUsableBoundaryStableProjection || conflict || !consensus)
    return std::nullopt;

  if (evidenceKind)
    *evidenceKind =
        IncludeRealizationEvidenceKind::BoundaryStableConsensusBCoverEnvelope;
  return consensus;
}

IncludePatch RefoldIncludeInsertionPlanner::BuildIncludeInsertionPatch(
    const RefoldModel::IncludeItem &inc, const diffutils::Hunk &h) const {
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
    StringRef bSlice =
        h.isInsertOnly()
            ? refoldSliceTokenEnvelope(bTokOff_, bSource_, h.bStart, h.bEnd)
            : refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                            h.bStart, h.bEnd);
    insertBytes = bSlice.str();
  } else {
    // Hardening: fail immediately if a structural hunk points outside the known
    // B-token universe.  Continuing would manufacture include bytes from an
    // invalid coordinate range and violate the deterministic proof contract.
    REFOLD_LOG_FATAL("include/patch",
                     "hunk bounds exceed token offset table for inc #{0}: "
                     "B[{1},{2}) requested, but bTokOff only has {3} entries",
                     inc.id, uBStart, uBEnd, numOffsets);
  }

  IncludePatch patch{&inc,  std::move(insertBytes), h.aStart, h.aEnd, h.bStart,
                     h.bEnd};

  // Pre-materialization include patches are internal staging objects only. Do
  // not certify a normalized accepted path here; the concrete preserving anchor
  // or realization class is chosen later during materialization, and only that
  // recertified result may cross a theorem-facing boundary.
  patch.proofSummary =
      proofLattice_.AcceptancePathClassifier().BuildIncludePatchProofSummary(
          /*realizedSurface=*/false, AcceptedPathKind::Unknown, &patch);

  return patch;
}

} // namespace refold
} // namespace clang

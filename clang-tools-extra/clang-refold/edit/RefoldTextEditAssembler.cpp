//===--- RefoldTextEditAssembler.cpp ----------------------------*- C++ -*-===//
//
// Final text-edit assembly and accepted-result audit attachment.
//
// This file implements the final byte-edit assembler: pending line-resync
// flushing, the accepted-proof audit of the final edit set, and materialized
// edit-map range recovery.
//
//===----------------------------------------------------------------------===//

#include "edit/RefoldTextEditAssembler.h"

#include "edit/RefoldTUEditPlanner.h"
#include "edit/RefoldTextEditCertifier.h"
#include "include/IncludeSpellingHelpers.h"
#include "include/RefoldIncludeReplayProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineControlEditHelpers.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldMacroWholeCoverPlanBuilder.h"
#include "model/RefoldPathIdentity.h"
#include "proof/RefoldAcceptedCandidateBuilder.h"
#include "proof/RefoldAcceptedResultPredicates.h"
#include "proof/RefoldOwnerRealizationProofBuilder.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldStructuralHunkTilingProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "support/RefoldDenseMapInfo.h"
#include "support/RefoldLog.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Pre-normalization and final carrier selections for one preserved-gap
/// structural tiling witness in the current physical source owner.
struct PreservedStructuralTilingObservation {
  MixedOwnerTilingWitness witness;
  std::set<uint32_t> plannedSegments;
  std::set<uint32_t> emittedSegments;
};

/// Return whether a durable structural segment belongs to the physical source
/// currently being assembled.
bool structuralSegmentBelongsToSource(
    const MixedOwnerTilingSegmentWitness &segment,
    const LineDirectiveInserter &lineDirs, StringRef absoluteSourceOwner,
    std::optional<uint64_t> ownerIncludeId) {
  if (!segment.sourceByteRangeKnown || segment.sourcePath.empty() ||
      segment.sourceIncludeId != ownerIncludeId)
    return false;
  return lineDirs.ToAbsolutePath(segment.sourcePath) == absoluteSourceOwner;
}

/// Return whether two structured owner identities are byte-for-byte equal.
/// Diagnostic owner signatures are deliberately not proof authority.
bool structuralOwnerIdentitiesAgree(const Owner &lhs, const Owner &rhs) {
  return lhs.kind == rhs.kind && lhs.includeId == rhs.includeId &&
         lhs.macroInvocationId == rhs.macroInvocationId &&
         lhs.macroDirectiveId == rhs.macroDirectiveId &&
         lhs.lineControlId == rhs.lineControlId &&
         lhs.pragmaId == rhs.pragmaId && lhs.condGroupId == rhs.condGroupId &&
         lhs.condArmId == rhs.condArmId;
}

void appendUniqueProtectedSourceAuthorizations(TextEdit &destination,
                                               const TextEdit &source);

/// Return whether a protected structure kind occupies a directive line.
///
/// `PragmaOperator` is an exact directly spelled raw-token interval. Every
/// other protected kind is an ordinary preprocessing directive whose scanner
/// interval covers its complete translated logical line.
bool protectedStructureOccupiesDirectiveLine(
    StructuralProtectedStructureKind kind) {
  return kind != StructuralProtectedStructureKind::Unknown &&
         kind != StructuralProtectedStructureKind::PragmaOperator;
}

/// Return whether one final edit can alter a preserved source interval.
///
/// Ordinary nonempty edits use half-open interval overlap. Insertions need a
/// separate rule because `[p,p)` never overlaps another half-open range: an
/// insertion at a directive's beginning or in its interior can nevertheless
/// make the introducer cease to be at beginning-of-line, extend an operand, or
/// split a continued logical line. An insertion at `sourceEnd` is outside a
/// newline-terminated directive, but still extends an end-of-file directive
/// whose complete scanner interval has no terminating newline.
bool textEditInterferesWithPreservedSourceInterval(
    const TextEdit &edit,
    const MixedOwnerTilingSegmentWitness &preservedSegment,
    StringRef originalFileText, const LangOptions &lexLang) {
  const uint64_t sourceBegin = preservedSegment.sourceBegin;
  const uint64_t sourceEnd = preservedSegment.sourceEnd;
  if (edit.end < edit.start || sourceEnd <= sourceBegin)
    return true;

  // An edit whose replacement bytes are exactly the bytes it replaces applies
  // no change, so it cannot disturb a preserved interval however much it
  // overlaps one: the emitted file is identical whether it is applied or not.
  // A pass-through directive re-emitted verbatim is such an edit, and it
  // otherwise collides with the very gap that preserves it, failing a
  // composition the tiling had already partitioned correctly.
  //
  // This is decided against the original bytes of the owner being assembled,
  // so it holds for header-owned edits on the same terms as TU-owned ones. An
  // edit that is not exactly the replaced text simply fails the comparison and
  // is judged by the overlap rules below, so the exemption can never be
  // claimed for an edit that does change something.
  if (edit.start <= edit.end && edit.end <= originalFileText.size() &&
      originalFileText.slice(edit.start, edit.end) == StringRef(edit.text))
    return false;

  if (edit.start != edit.end)
    return edit.start < sourceEnd && sourceBegin < edit.end;
  if (edit.start == sourceBegin &&
      preservedSegment.protectedStructureKind ==
          StructuralProtectedStructureKind::PragmaOperator) {
    return false;
  }
  if (sourceBegin <= edit.start && edit.start < sourceEnd)
    return true;
  if (edit.start != sourceEnd ||
      !preservedSegment.protectedPreprocessingStructure ||
      !protectedStructureOccupiesDirectiveLine(
          preservedSegment.protectedStructureKind)) {
    return false;
  }
  return !preservedDirectiveHasTerminatingNewline(
      originalFileText, sourceBegin, sourceEnd, lexLang);
}

/// Return whether two copies name the same durable structural partition for
/// final emission purposes.
bool structuralTilingWitnessesAgree(
    const MixedOwnerTilingWitness &lhs,
    const MixedOwnerTilingWitness &rhs) {
  const bool lhsPreservesStructure =
      lhs.reason == StructuralTilingReason::PreservedPreprocessingStructure ||
      lhs.reason ==
          StructuralTilingReason::MixedRealizersAndPreservedStructure;
  const bool rhsPreservesStructure =
      rhs.reason == StructuralTilingReason::PreservedPreprocessingStructure ||
      rhs.reason ==
          StructuralTilingReason::MixedRealizersAndPreservedStructure;
  if (lhsPreservesStructure != rhsPreservesStructure ||
      (lhsPreservesStructure &&
       (!structuralPreservedSourceTopologyIsComplete(lhs) ||
        !structuralPreservedSourceTopologyIsComplete(rhs)))) {
    return false;
  }

  if (lhs.witnessId != rhs.witnessId || lhs.reason != rhs.reason ||
      lhs.originalAStart != rhs.originalAStart ||
      lhs.originalAEnd != rhs.originalAEnd ||
      lhs.originalBStart != rhs.originalBStart ||
      lhs.originalBEnd != rhs.originalBEnd ||
      lhs.uniquePartition != rhs.uniquePartition ||
      lhs.stateTransitionsComposed != rhs.stateTransitionsComposed ||
      lhs.preservedGapsDisjointFromEdits !=
          rhs.preservedGapsDisjointFromEdits ||
      lhs.stateSummariesComposed != rhs.stateSummariesComposed ||
      lhs.ownerBoundariesComposed != rhs.ownerBoundariesComposed ||
      lhs.targetTokenStreamComposed != rhs.targetTokenStreamComposed ||
      lhs.compositionEdgesProven != rhs.compositionEdgesProven ||
      lhs.tokenSegmentCount != rhs.tokenSegmentCount ||
      lhs.stateGapCount != rhs.stateGapCount ||
      lhs.protectedStructureGapCount != rhs.protectedStructureGapCount ||
      lhs.preservedInPlaceGapCount != rhs.preservedInPlaceGapCount ||
      lhs.distinctRealizerCount != rhs.distinctRealizerCount ||
      lhs.physicalSourceRunCount != rhs.physicalSourceRunCount ||
      lhs.physicalSourceRunsProven != rhs.physicalSourceRunsProven ||
      lhs.uniqueMinimumFragmentPartition !=
          rhs.uniqueMinimumFragmentPartition ||
      lhs.uniqueBoundaryProjectionProven !=
          rhs.uniqueBoundaryProjectionProven ||
      lhs.boundaryProjectionCount != rhs.boundaryProjectionCount ||
      lhs.boundaryProjections.size() != rhs.boundaryProjections.size() ||
      lhs.sharedEmptyBEnvelopeProven != rhs.sharedEmptyBEnvelopeProven ||
      lhs.sharedEmptyBBoundary != rhs.sharedEmptyBBoundary ||
      lhs.preservedStateChainComposed != rhs.preservedStateChainComposed ||
      lhs.sourceByteCoverComplete != rhs.sourceByteCoverComplete ||
      lhs.preservedGapSourceOrderProven !=
          rhs.preservedGapSourceOrderProven ||
      lhs.preservedGapsDisjointFromTokenSegments !=
          rhs.preservedGapsDisjointFromTokenSegments ||
      lhs.globalTargetPPTokenSignature != rhs.globalTargetPPTokenSignature ||
      lhs.globalCompositionSignature != rhs.globalCompositionSignature ||
      lhs.edges.size() != rhs.edges.size()) {
    return false;
  }

  for (size_t i = 0; i < lhs.boundaryProjections.size(); ++i) {
    const StructuralBoundaryProjectionWitness &left =
        lhs.boundaryProjections[i];
    const StructuralBoundaryProjectionWitness &right =
        rhs.boundaryProjections[i];
    if (left.aTokenBoundary != right.aTokenBoundary ||
        left.lowerBTokenBoundary != right.lowerBTokenBoundary ||
        left.upperBTokenBoundary != right.upperBTokenBoundary ||
        left.bTokenBoundary != right.bTokenBoundary ||
        left.uniqueProjection != right.uniqueProjection) {
      return false;
    }
  }

  for (size_t i = 0; i < lhs.edges.size(); ++i) {
    const MixedOwnerTilingSegmentWitness &left = lhs.edges[i];
    const MixedOwnerTilingSegmentWitness &right = rhs.edges[i];
    if (left.parentTilingWitnessId != right.parentTilingWitnessId ||
        left.segmentIndex != right.segmentIndex ||
        left.sourceOrderPosition != right.sourceOrderPosition ||
        left.kind != right.kind ||
        left.aStart != right.aStart || left.aEnd != right.aEnd ||
        left.bStart != right.bStart || left.bEnd != right.bEnd ||
        left.zeroTokenStateGap != right.zeroTokenStateGap ||
        left.gapDisposition != right.gapDisposition ||
        left.protectedPreprocessingStructure !=
            right.protectedPreprocessingStructure ||
        left.allowEmptyBEnvelope != right.allowEmptyBEnvelope ||
        left.ownerClosureComplete != right.ownerClosureComplete ||
        left.ownerIdentityKnown != right.ownerIdentityKnown ||
        !structuralOwnerIdentitiesAgree(left.ownerIdentity,
                                        right.ownerIdentity) ||
        left.sourceByteRangeKnown != right.sourceByteRangeKnown ||
        left.sourcePath != right.sourcePath ||
        left.sourceIncludeId != right.sourceIncludeId ||
        left.sourceBegin != right.sourceBegin ||
        left.sourceEnd != right.sourceEnd ||
        left.protectedStructureIdentityRecorded !=
            right.protectedStructureIdentityRecorded ||
        left.protectedStructureKind != right.protectedStructureKind ||
        left.producerIdentityKind != right.producerIdentityKind ||
        left.producerItemId != right.producerItemId ||
        left.producerConditionalGroupId !=
            right.producerConditionalGroupId ||
        left.producerConditionalArmId != right.producerConditionalArmId ||
        left.sourceBytesPreservedUnchanged !=
            right.sourceBytesPreservedUnchanged ||
        left.protectedStructurePreservedOutsideSegment !=
            right.protectedStructurePreservedOutsideSegment ||
        left.macroStatePlacementInsensitiveProven !=
            right.macroStatePlacementInsensitiveProven ||
        left.ownerSignature != right.ownerSignature ||
        left.sourceSignature != right.sourceSignature ||
        left.producerPathSignature != right.producerPathSignature ||
        left.targetPPTokenSignature != right.targetPPTokenSignature) {
      return false;
    }
  }
  return true;
}

/// Return the unique observation for `witness`, creating it when necessary.
/// An already-observed witness id must retain the same durable partition.
PreservedStructuralTilingObservation *findOrCreateStructuralTilingObservation(
    SmallVectorImpl<PreservedStructuralTilingObservation> &observations,
    const MixedOwnerTilingWitness &witness, std::string &failure) {
  for (PreservedStructuralTilingObservation &candidate : observations) {
    if (candidate.witness.witnessId != witness.witnessId)
      continue;
    if (!structuralTilingWitnessesAgree(candidate.witness, witness)) {
      failure = "one structural witness id resolved to incompatible durable "
                "structural partitions";
      return nullptr;
    }
    return &candidate;
  }

  observations.push_back(PreservedStructuralTilingObservation{witness, {}, {}});
  return &observations.back();
}

/// Resolve one structural carrier to its durable tiling witness and exact
/// token-segment index.
///
/// Mixed-owner theorem carriers store the witness directly. Ordinary TU owner
/// realizations instead store the validated witness id/segment key in
/// `TUOwnerRealizationCarrierWitness`; resolve that key through the durable
/// planner ledger so final edit normalization cannot hide direct TU segments
/// merely by retaining their owner-realization proof class.
bool resolveStructuralTilingCarrier(
    const AcceptedResultCandidate &carrier,
    ArrayRef<MixedOwnerTilingWitness> durableWitnesses,
    const MixedOwnerTilingWitness *&resolvedWitness,
    uint32_t &resolvedSegmentIndex, std::string &failure) {
  resolvedWitness = nullptr;
  resolvedSegmentIndex = 0;
  const ProofSummary &summary = carrier.proofSummary;

  if (summary.hasMixedOwnerTilingWitness) {
    if (!summary.hasMixedOwnerTilingSegmentSelection ||
        summary.mixedOwnerTilingSegmentIndex >=
            summary.mixedOwnerTilingWitness.edges.size()) {
      failure = "mixed-owner carrier lacks an exact emitted-segment selection";
      return false;
    }
    resolvedWitness = &summary.mixedOwnerTilingWitness;
    resolvedSegmentIndex = summary.mixedOwnerTilingSegmentIndex;
  }

  // Resolve one witness-id/segment key through the durable planner ledger and
  // fold it into whatever an earlier source already resolved.  Shared by the
  // direct-TU key and the key a macro-state repair inherits from it, so the two
  // cannot drift apart in how strictly they resolve.
  auto resolveLedgerKey = [&](uint64_t witnessId, uint32_t segmentIndex,
                              StringRef source) -> bool {
    const MixedOwnerTilingWitness *ledgerWitness = nullptr;
    for (const MixedOwnerTilingWitness &candidate : durableWitnesses) {
      if (candidate.witnessId != witnessId)
        continue;
      if (ledgerWitness) {
        failure = (source + " structural witness id is not unique in the "
                            "durable tiling ledger")
                      .str();
        return false;
      }
      ledgerWitness = &candidate;
    }
    if (!ledgerWitness || segmentIndex >= ledgerWitness->edges.size()) {
      failure = (source + " structural-segment key does not resolve through "
                          "the durable tiling ledger")
                    .str();
      return false;
    }

    if (resolvedWitness &&
        (resolvedSegmentIndex != segmentIndex ||
         !structuralTilingWitnessesAgree(*resolvedWitness, *ledgerWitness))) {
      failure = (source + " structural segment binding conflicts with the "
                          "binding another carrier source resolved")
                    .str();
      return false;
    }

    resolvedWitness = ledgerWitness;
    resolvedSegmentIndex = segmentIndex;
    return true;
  };

  const bool hasStructuralTUCarrier =
      summary.hasOwnerRealizationWitness &&
      summary.ownerRealizationWitness.hasTUCarrierWitness &&
      summary.ownerRealizationWitness.tuCarrierWitness
          .hasStructuralSegmentBinding;
  if (hasStructuralTUCarrier) {
    const TUOwnerRealizationCarrierWitness &tuCarrier =
        summary.ownerRealizationWitness.tuCarrierWitness;
    if (!tuCarrier.structuralSegmentBindingValidated ||
        tuCarrier.structuralWitnessId == 0) {
      failure = "TU carrier names an incomplete structural-segment binding";
      return false;
    }
    if (!resolveLedgerKey(tuCarrier.structuralWitnessId,
                          tuCarrier.structuralSegmentIndex, "TU carrier"))
      return false;
  }

  // A specialized repair carrier that replaced a direct-TU carrier keeps that
  // carrier's segment key.  The repair changed the edit's text and may have
  // widened it to a line boundary, which is why it no longer claims the
  // byte-span theorem -- but the segment it realizes is the same one, and the
  // caller rechecks the widened edit against every preserved gap.
  if (summary.hasInheritedStructuralSegmentBinding) {
    if (summary.inheritedStructuralWitnessId == 0) {
      failure = "repair carrier names an incomplete inherited "
                "structural-segment binding";
      return false;
    }
    if (!resolveLedgerKey(summary.inheritedStructuralWitnessId,
                          summary.inheritedStructuralSegmentIndex,
                          "inherited repair carrier"))
      return false;
  }

  return true;
}

/// Record one structural carrier selected before or after edit normalization.
/// Non-structural and non-preserved-gap carriers are intentionally ignored.
bool observePreservedStructuralTilingCarrier(
    const AcceptedResultCandidate &carrier, bool planned,
    ArrayRef<MixedOwnerTilingWitness> durableWitnesses,
    const LineDirectiveInserter &lineDirs, StringRef absoluteSourceOwner,
    std::optional<uint64_t> ownerIncludeId,
    SmallVectorImpl<PreservedStructuralTilingObservation> &observations,
    std::string &failure) {
  const MixedOwnerTilingWitness *witness = nullptr;
  uint32_t segmentIndex = 0;
  if (!resolveStructuralTilingCarrier(carrier, durableWitnesses, witness,
                                      segmentIndex, failure)) {
    return false;
  }
  if (!witness || witness->preservedInPlaceGapCount == 0)
    return true;
  if (segmentIndex >= witness->edges.size()) {
    failure = "structural carrier segment index is out of range";
    return false;
  }

  const bool deleteOnlyWitness =
      witness->originalBStart == witness->originalBEnd;
  const MixedOwnerTilingSegmentWitness &selected =
      witness->edges[segmentIndex];
  if (selected.kind != MixedOwnerTilingEdgeKind::TokenSegment ||
      !structuralSegmentBelongsToSource(selected, lineDirs,
                                        absoluteSourceOwner, ownerIncludeId)) {
    failure = "structural carrier selected a non-token or different-source "
              "segment";
    return false;
  }

  if (deleteOnlyWitness) {
    if (!witness->sharedEmptyBEnvelopeProven ||
        witness->sharedEmptyBBoundary != witness->originalBStart ||
        !selected.allowEmptyBEnvelope ||
        selected.bStart != witness->sharedEmptyBBoundary ||
        selected.bEnd != witness->sharedEmptyBBoundary) {
      failure = "structural deletion carrier lacks the shared empty-B "
                "segment theorem";
      return false;
    }
  } else {
    if (witness->sharedEmptyBEnvelopeProven ||
        witness->preservedStateChainComposed ||
        selected.bEnd < selected.bStart ||
        ((selected.bStart == selected.bEnd) !=
         selected.allowEmptyBEnvelope) ||
        (selected.allowEmptyBEnvelope &&
         !witness->uniqueBoundaryProjectionProven)) {
      failure = "structural replacement carrier has an invalid projected B "
                "segment envelope";
      return false;
    }
  }

  PreservedStructuralTilingObservation *observation =
      findOrCreateStructuralTilingObservation(observations, *witness, failure);
  if (!observation)
    return false;

  std::set<uint32_t> &segments =
      planned ? observation->plannedSegments : observation->emittedSegments;
  segments.insert(segmentIndex);
  return true;
}

} // namespace

namespace {

/// Return the one protected construct an edit exists solely to rewrite in
/// place, or nothing when the edit is not that.
///
/// This is a typed classification, not a shape test.  The edit must carry
/// exactly one emission capability, it must be the include-directive rewrite
/// authority, and the edit's own byte range must be exactly the construct that
/// authority names.  An edit doing anything else in addition -- a wider range,
/// a second capability, a macro-state carry -- is not classified, because then
/// dropping it would discard work no other edit was shown to do.
std::optional<ProtectedSourceEditAuthorization>
soleIncludeDirectiveRewriteObligation(const TextEdit &edit) {
  if (edit.protectedSourceAuthorizations.size() != 1)
    return std::nullopt;
  const ProtectedSourceEditAuthorization &authorization =
      edit.protectedSourceAuthorizations.front();
  if (authorization.authority !=
      ProtectedSourceEditAuthorityKind::IncludeDirectiveRewrite)
    return std::nullopt;
  if (authorization.begin != edit.start || authorization.end != edit.end)
    return std::nullopt;
  return authorization;
}

/// Return whether `edit` proved it realizes `construct` away rather than
/// re-emitting it.
///
/// The evidence is the planner-minted elimination witness, matched on exact
/// construct identity.  An edit that recorded none -- because its planner could
/// not say, construct by construct, what its replacement carries through --
/// answers no, which is the direction that keeps another edit's obligation
/// alive rather than silently dropping it.
bool editEliminatesProtectedConstruct(
    const TextEdit &edit, const ProtectedSourceEditAuthorization &construct) {
  return llvm::any_of(edit.eliminatedProtectedConstructs,
                      [&](const EliminatedProtectedConstruct &eliminated) {
                        return eliminated.kind == construct.structureKind &&
                               eliminated.begin == construct.begin &&
                               eliminated.end == construct.end;
                      });
}

/// Return whether `text` spells any include-family directive.
///
/// This reads the bytes that will actually be emitted, which is what makes it
/// a check rather than a second opinion about a claim.  An elimination witness
/// says the planner realized a construct away; enumerating the ways a
/// replacement can re-emit one is the planner's own bookkeeping, and
/// bookkeeping falls behind the code it describes.  Scanning the replacement
/// cannot: a mechanism that puts an include back, however it arrives, is
/// visible here.
///
/// An incomplete scan answers "yes".  Absence has to be proven to be useful,
/// and a scanner that could not account for the whole text has proven nothing.
bool textSpellsAnyIncludeDirective(StringRef text,
                                   const LangOptions &lexLang) {
  const PreprocessingDirectiveScanResult scan =
      scanPreprocessingDirectives(text, lexLang);
  if (!scan.IsComplete())
    return true;
  for (const PreprocessingDirectiveLine &directive : scan.directives) {
    if (directive.headKind != PreprocessingDirectiveHeadKind::Identifier)
      continue;
    // The scanner normalizes escaped newlines out of the head token, so these
    // comparisons see `inc\<newline>lude` as `include`.
    if (directive.keyword == "include" || directive.keyword == "include_next" ||
        directive.keyword == "import")
      return true;
  }
  return false;
}

/// Drop every include-directive rewrite whose construct another edit proved it
/// realizes away.
///
/// Two edits can be individually proven and still collide, when one replaces a
/// range that strictly contains the other.  A header materialized into its
/// parent is the case that produces it: a cover edit realizes a run of the body
/// and a nested `#include`/`#include_next` inside that run separately stages a
/// rewrite of its own directive, because a child include cannot be replayed
/// from a materialized parent with the original search stack.  Neither is
/// wrong, and there is no order that applies both.
///
/// This is a *named* discharge, restricted to that one pair of theorems, and
/// deliberately not a general law about nested edits.  Source containment
/// proves only that the two edits cannot both be applied, which is a statement
/// about the applicator; it says nothing about the surviving payload.  Nor does
/// the outer edit's complete-source-closure authority settle it on its own:
/// that authority proves the outer accounted for every construct it crossed,
/// and re-emitting a construct's source accounts for it just as realizing its
/// tokens does.  Only the second discharges a rewrite obligation, so the two
/// are told apart by the planner's own elimination witness:
///
/// \code
///   inner's sole capability is IncludeDirectiveRewrite over construct C,
///     and inner replaces exactly C
///   outer holds a complete-source-closure capability over that same C
///   outer's elimination witness names C: its replacement realized C away
///     rather than carrying C's source through
///   outer strictly contains inner in source bytes
///   ------------------------------------------------------------------
///   the rewritten directive is not emitted at all, and the obligation to
///   rewrite it is discharged by the outer closure theorem
/// \endcode
///
/// The elimination witness is then checked rather than believed.  It is the
/// complement of the ranges the planner says its replacement re-emits, so it is
/// only as complete as that enumeration -- and an enumeration maintained beside
/// the code that builds the replacement is exactly the kind of invariant that
/// goes stale silently.  So the outer's replacement is scanned, and the
/// discharge is refused unless it spells no include directive at all.  A future
/// mechanism that puts an include back into that payload therefore stops the
/// discharge instead of quietly invalidating it.
///
/// The construct is matched by exact identity, never by containment, so the
/// outer must own the very interval the inner was authorized over.  The inner's
/// capability is then moved onto the outer, because the outer really does
/// consume those protected bytes and the emission audit must still see an
/// authority for them: the obligation is transferred, not deleted.
///
/// Everything else keeps failing closed.  A zero-width insertion owns no byte
/// another edit can be said to replace; two edits claiming the same range are a
/// genuine ambiguity; an edit whose planner minted no elimination witness is
/// never treated as having eliminated anything.
///
/// Returns how many edits were dropped.
size_t dropSubsumedNormalizedTextEdits(SmallVectorImpl<TextEdit> &norm,
                                       StringRef emissionOwner,
                                       const LangOptions &lexLang) {
  SmallVector<bool, 8> subsumed(norm.size(), false);
  SmallVector<int, 8> dischargedBy(norm.size(), -1);
  for (size_t inner = 0; inner < norm.size(); ++inner) {
    const TextEdit &innerEdit = norm[inner];
    if (innerEdit.start >= innerEdit.end)
      continue;
    const std::optional<ProtectedSourceEditAuthorization> obligation =
        soleIncludeDirectiveRewriteObligation(innerEdit);
    if (!obligation)
      continue;

    for (size_t outer = 0; outer < norm.size(); ++outer) {
      if (outer == inner)
        continue;
      const TextEdit &outerEdit = norm[outer];
      if (outerEdit.start >= outerEdit.end)
        continue;
      if (outerEdit.start > innerEdit.start || innerEdit.end > outerEdit.end)
        continue;
      if (outerEdit.start == innerEdit.start && outerEdit.end == innerEdit.end)
        continue;
      if (!editEliminatesProtectedConstruct(outerEdit, *obligation))
        continue;
      const bool outerOwnsConstruct = llvm::any_of(
          outerEdit.protectedSourceAuthorizations,
          [&](const ProtectedSourceEditAuthorization &authorization) {
            return authorizationIsCompleteSourceClosure(authorization.authority) &&
                   authorization.structureKind == obligation->structureKind &&
                   authorization.begin == obligation->begin &&
                   authorization.end == obligation->end;
          });
      if (!outerOwnsConstruct)
        continue;
      if (textSpellsAnyIncludeDirective(outerEdit.text, lexLang))
        continue;

      REFOLD_LOG_TRACE(
          "edits/apply",
          "discharging include-directive rewrite [{0},{1}) in {2}: edit "
          "[{3},{4}) holds a source-closure capability over the same {5} "
          "construct and proved it realized away",
          innerEdit.start, innerEdit.end, emissionOwner, outerEdit.start,
          outerEdit.end, toString(obligation->structureKind));
      subsumed[inner] = true;
      dischargedBy[inner] = static_cast<int>(outer);
      break;
    }
  }

  // Decide before moving anything.  Rebuilding unconditionally and assigning
  // back only when something was dropped would leave `norm` holding moved-from
  // edits in the common case, which reads downstream as an edit with no text,
  // no carriers and no authorizations.
  const size_t dropped = llvm::count(subsumed, true);
  if (dropped == 0)
    return 0;

  // Move each discharged edit's capability onto the edit that discharged it.
  // The outer edit consumes those protected bytes whether or not the inner one
  // survives, so the authority for them has to survive with it.
  for (size_t i = 0; i < norm.size(); ++i) {
    if (!subsumed[i] || dischargedBy[i] < 0)
      continue;
    appendUniqueProtectedSourceAuthorizations(
        norm[static_cast<size_t>(dischargedBy[i])], norm[i]);
  }

  SmallVector<TextEdit, 8> kept;
  kept.reserve(norm.size() - dropped);
  for (size_t i = 0; i < norm.size(); ++i) {
    if (subsumed[i])
      continue;
    kept.push_back(std::move(norm[i]));
  }
  norm = std::move(kept);
  return dropped;
}

void appendUniqueProtectedSourceAuthorizations(TextEdit &destination,
                                                const TextEdit &source) {
  for (const ProtectedSourceEditAuthorization &authorization :
       source.protectedSourceAuthorizations)
    appendUniqueProtectedSourceAuthorization(destination, authorization);
}

} // namespace

bool RefoldTextEditAssembler::AuditAcceptedEditProofs(
    ArrayRef<TextEdit> edits, StringRef emissionStage, StringRef emissionOwner,
    std::optional<uint64_t> ownerIncludeId, ArrayRef<TextEdit> plannedEdits,
    StringRef originalFileText) const {
  // Centralize the final accepted-proof audit at the last byte-edit boundary.
  // Earlier builders may still queue candidates path-by-path, but once the
  // normalized edit set is known the applicator must see a theorem carrier for
  // every emitted edit and a composition law for every multi-carrier edit.  The
  // state-transition audit is layered onto the same boundary:
  // any edit path that already routed state through the gateway must have a
  // typed, component-named suffix-stability witness or a named terminal failure
  // before non-terminal bytes can be emitted.
  if (!theoremAuditService_.AuditStateTransitionGatewayProofs(emissionStage,
                                                              emissionOwner))
    return false;
  if (!textEditCertifier_.AuditGlobalSourceEditInvariant(
          edits, emissionStage, emissionOwner, ownerIncludeId,
          originalFileText))
    return false;

  for (const TextEdit &edit : edits) {
    if (!EmittedTextEditHasDischargedAcceptedResults(edit, emissionStage,
                                                     emissionOwner))
      return false;
  }

  return PreservedStructuralGapsRemainOutsideEmittedEdits(
      plannedEdits, edits, emissionStage, emissionOwner, ownerIncludeId,
      originalFileText);
}

bool RefoldTextEditAssembler::RejectPreservedStructuralGapAudit(
    StringRef emissionStage, StringRef detail) const {
  theoremAuditService_.NoteTheoremAuditViolation(detail);
  terminalSink_.RequestTerminalFallback(
      MakeTerminalFallbackProofFailure(
          TerminalFallbackObligationKind::EmissionEditSetComposable,
          TerminalFallbackFailureReason::UncomposableEmissionEditSet),
      emissionStage, detail);
  REFOLD_LOG_TRACE("proof/compose", "{0}", detail);
  return false;
}

bool RefoldTextEditAssembler::PreservedStructuralGapsRemainOutsideEmittedEdits(
    ArrayRef<TextEdit> plannedEdits, ArrayRef<TextEdit> emittedEdits,
    StringRef emissionStage, StringRef emissionOwner,
    std::optional<uint64_t> ownerIncludeId,
    StringRef originalFileText) const {
  // Token tiling happens before duplicate-edit merging, resync widening, and
  // conservative TU closure. The final audit snapshots structural segment
  // obligations from the pre-normalization edit set and checks those
  // obligations against the final physical edit set.  Looking only at final
  // carriers would be unsound: a later closure can replace several structural
  // carriers with one new carrier and thereby erase the very witness whose
  // preserved source gap it consumed.
  SmallVector<PreservedStructuralTilingObservation, 4> observedTilings;
  const StringRef sourceOwner =
      emissionOwner.empty() ? model_.GetSourcePath() : emissionOwner;
  const std::string absoluteSourceOwner =
      lineDirs_.ToAbsolutePath(sourceOwner);

  // Capture the exact structural obligations before normalization can merge,
  // widen, or replace their carriers.
  for (const TextEdit &edit : plannedEdits) {
    for (const std::shared_ptr<const AcceptedResultCandidate> &carrier :
         edit.acceptedResults) {
      if (!carrier)
        continue;
      std::string failure;
      if (!observePreservedStructuralTilingCarrier(
              *carrier, /*planned=*/true, mixedOwnerTilingWitnesses_,
              lineDirs_, absoluteSourceOwner, ownerIncludeId, observedTilings,
              failure)) {
        return RejectPreservedStructuralGapAudit(emissionStage, failure);
      }
    }
  }

  // Record which structural segment carriers survived normalization.  A
  // replacement carrier introduced by a later closure cannot silently stand in
  // for the original segment set, even when its own local theorem is valid.
  for (const TextEdit &edit : emittedEdits) {
    for (const std::shared_ptr<const AcceptedResultCandidate> &carrier :
         edit.acceptedResults) {
      if (!carrier)
        continue;
      std::string failure;
      if (!observePreservedStructuralTilingCarrier(
              *carrier, /*planned=*/false, mixedOwnerTilingWitnesses_,
              lineDirs_, absoluteSourceOwner, ownerIncludeId, observedTilings,
              failure)) {
        return RejectPreservedStructuralGapAudit(emissionStage, failure);
      }
    }
  }

  for (const PreservedStructuralTilingObservation &observed : observedTilings) {
    const MixedOwnerTilingWitness &witness = observed.witness;
    if (!witness.uniquePartition || !witness.stateTransitionsComposed ||
        !witness.targetTokenStreamComposed ||
        !witness.preservedGapSourceOrderProven ||
        !witness.preservedGapsDisjointFromTokenSegments ||
        !witness.preservedGapsDisjointFromEdits) {
      return RejectPreservedStructuralGapAudit(
          emissionStage,
          "structural witness lacks the ordered target/state composition or "
          "preserved-gap disjointness theorem");
    }

    const bool preservesPreprocessingStructure =
        witness.reason ==
            StructuralTilingReason::PreservedPreprocessingStructure ||
        witness.reason == StructuralTilingReason::
                              MixedRealizersAndPreservedStructure;
    if (preservesPreprocessingStructure &&
        !structuralPreservedSourceTopologyIsComplete(witness)) {
      return RejectPreservedStructuralGapAudit(
          emissionStage,
          "structural witness lacks the complete preserved-source topology "
          "and no-replay theorem");
    }

    const bool deleteOnlyWitness =
        witness.originalBStart == witness.originalBEnd;
    if (deleteOnlyWitness) {
      if (!witness.sharedEmptyBEnvelopeProven ||
          witness.sharedEmptyBBoundary != witness.originalBStart ||
          !witness.preservedStateChainComposed ||
          witness.uniqueBoundaryProjectionProven ||
          witness.boundaryProjectionCount != 0 ||
          !witness.boundaryProjections.empty()) {
        return RejectPreservedStructuralGapAudit(
            emissionStage,
            "structural deletion witness lacks the shared empty-B theorem or "
            "carries replacement-only projection authority");
      }
    } else {
      if (witness.sharedEmptyBEnvelopeProven ||
          witness.preservedStateChainComposed) {
        return RejectPreservedStructuralGapAudit(
            emissionStage,
            "structural replacement witness carries delete-only empty-B "
            "authority");
      }

      const bool requiresBoundaryProjectionTheorem =
          structuralReplacementRequiresBoundaryProjection(witness);
      if (requiresBoundaryProjectionTheorem) {
        if (!structuralReplacementBoundaryProjectionIsComplete(witness)) {
          return RejectPreservedStructuralGapAudit(
              emissionStage,
              "structural replacement witness lacks the complete unique "
              "A-to-B boundary-projection theorem");
        }
      } else if (witness.uniqueBoundaryProjectionProven ||
                 witness.boundaryProjectionCount != 0 ||
                 !witness.boundaryProjections.empty()) {
        return RejectPreservedStructuralGapAudit(
            emissionStage,
            "structural replacement without a preserved preprocessing seam "
            "carries unrelated boundary-projection authority");
      }
    }

    std::set<uint32_t> expectedSegments;
    bool sawCurrentSourcePreservedGap = false;
    for (const MixedOwnerTilingSegmentWitness &segment : witness.edges) {
      if (!structuralSegmentBelongsToSource(
              segment, lineDirs_, absoluteSourceOwner, ownerIncludeId)) {
        continue;
      }

      if (segment.kind == MixedOwnerTilingEdgeKind::TokenSegment) {
        expectedSegments.insert(segment.segmentIndex);
        continue;
      }
      if (segment.kind != MixedOwnerTilingEdgeKind::StateGap ||
          segment.gapDisposition !=
              StructuralGapDisposition::PreservedInPlace ||
          !segment.sourceBytesPreservedUnchanged ||
          !segment.ownerIdentityKnown ||
          segment.ownerIdentity.kind == OwnerKind::Unknown ||
          segment.sourceEnd <= segment.sourceBegin ||
          (segment.protectedPreprocessingStructure &&
           (!segment.protectedStructureIdentityRecorded ||
            segment.protectedStructureKind ==
                StructuralProtectedStructureKind::Unknown))) {
        return RejectPreservedStructuralGapAudit(
            emissionStage,
            "structural state chain contains a non-preserved or "
            "malformed current-source gap");
      }

      sawCurrentSourcePreservedGap = true;
      for (const TextEdit &edit : emittedEdits) {
        if (textEditInterferesWithPreservedSourceInterval(
                edit, segment, originalFileText, lexLang_)) {
          return RejectPreservedStructuralGapAudit(
              emissionStage,
              llvm::formatv(
                  "normalized edit [{0},{1}) interferes with "
                  "PreservedInPlace gap "
                  "[{2},{3}) for structural witness {4}",
                  edit.start, edit.end, segment.sourceBegin, segment.sourceEnd,
                  witness.witnessId)
                  .str());
        }
      }
    }

    if (expectedSegments.empty() ||
        observed.plannedSegments != expectedSegments) {
      return RejectPreservedStructuralGapAudit(
          emissionStage,
          "pre-normalization edit set does not contain exactly the "
          "structural token segments for the current source "
          "owner");
    }
    if (observed.emittedSegments != observed.plannedSegments) {
      return RejectPreservedStructuralGapAudit(
          emissionStage,
          "normalization dropped or manufactured a structural "
          "token-segment carrier");
    }
    if ((witness.reason ==
             StructuralTilingReason::PreservedPreprocessingStructure ||
         witness.reason == StructuralTilingReason::
                               MixedRealizersAndPreservedStructure) &&
        !sawCurrentSourcePreservedGap) {
      return RejectPreservedStructuralGapAudit(
          emissionStage,
          "structure-preserving witness has no current-source "
          "PreservedInPlace gap");
    }
  }

  return true;
}

bool RefoldTextEditAssembler::EmittedTextEditHasDischargedAcceptedResults(
    const TextEdit &edit, StringRef emissionStage,
    StringRef emissionOwner) const {
  const StringRef owner =
      emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner;
  ++theoremAudit_.emittedNonTerminalEdits;

  auto carrierIsEmissionDischarged =
      [&](const AcceptedResultCandidate &carrier) -> bool {
    ++theoremAudit_.emittedCarriers;
    theoremAuditService_.AuditAcceptedResultCandidateForLegacyAuthority(
        carrier, "EmittedTextEditHasDischargedAcceptedResults");

    // Non-terminal byte edits must never carry terminal/out-of-domain results.
    // After these structural exclusions, the single normalizer is the only
    // authority for deciding whether the candidate's construction path and
    // proof-family metadata discharge one final theorem proof class.
    if (carrier.kind == AcceptedResultCandidateKind::Unknown) {
      ++theoremAudit_.emittedUnknownClassCarriers;
      theoremAuditService_.NoteTheoremAuditViolation(
          "emitted carrier had unknown candidate kind");
      return false;
    }
    if (carrier.kind == AcceptedResultCandidateKind::TerminalOutOfDomain) {
      ++theoremAudit_.emittedOutOfDomainCarriers;
      theoremAuditService_.NoteTheoremAuditViolation(
          "non-terminal emitted edit carried explicit out-of-domain result");
      return false;
    }

    const std::optional<TheoremProofClass> theoremProof =
        proofSummaryBuilder_.NormalizeAcceptedProof(carrier);
    if (!theoremProof) {
      if (carrier.proofSummary.theoremClass == TheoremProofClass::Unknown) {
        ++theoremAudit_.emittedUnknownClassCarriers;
        theoremAuditService_.NoteTheoremAuditViolation(
            "emitted carrier did not declare a final theorem proof class");
      } else if (carrier.proofSummary.inventory.support !=
                     AcceptanceSupportKind::ExplicitProofBacked ||
                 carrier.proofSummary.theoremDomain.kind ==
                     TheoremDomainKind::TransitionalGap ||
                 carrier.proofSummary.completeness.coverage ==
                     CompletenessCoverageKind::TransitionalGap) {
        ++theoremAudit_.emittedTransitionalTheoremCarriers;
        theoremAuditService_.NoteTheoremAuditViolation(
            "emitted carrier remained transitional at the byte-edit boundary");
      } else {
        theoremAuditService_.NoteTheoremAuditViolation(
            "emitted carrier failed final proof normalization at the "
            "byte-edit boundary");
      }
      ++theoremAudit_.emittedUndischargedCarriers;
      return false;
    }

    ++theoremAudit_.emittedDeclaredClassCarriers;
    ++theoremAudit_.emittedDischargedCarriers;
    REFOLD_LOG_TRACE(
        "proof/normalize",
        "emitted carrier normalized theoremProof={0} kind={1} path={2}",
        *theoremProof, carrier.kind,
        carrier.proofSummary.inventory.currentPath);
    return true;
  };

  // Every emitted non-terminal edit must carry at least one selected
  // accepted-result witness. An unannotated edit is an emission artifact, not a
  // theorem-facing refolding result.
  if (edit.acceptedResults.empty()) {
    const TerminalFallbackProofFailure failure =
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionArtifactDischarged,
            TerminalFallbackFailureReason::UndischargedEmissionArtifact,
            TerminalFallbackFailureContext::ForStateComponent(
                "TextEdit.acceptedResults"));
    const bool strictRejected =
        theoremAuditService_.RejectNoLegacyAuditFindingIfStrict(
            RefoldTheoremAudit::MakeLegacyAuditEvidence(
                LegacyPathKind::PathSpecificProofMirror, emissionStage,
                llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has no "
                              "AcceptedResultCandidate / ProofSummary carrier",
                              edit.start, edit.end, owner)
                    .str()),
            failure);
    theoremAuditService_.NoteTheoremAuditViolation(
        "emitted edit reached the byte-edit boundary "
        "without accepted-result carriers");
    if (!strictRejected) {
      terminalSink_.RequestTerminalFallback(
          failure, emissionStage,
          llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has no "
                        "normalized accepted-result carriers",
                        edit.start, edit.end, owner)
              .str());
    }
    return false;
  }

  // Validate every carrier attached to the edit. Multiple carriers are allowed
  // when one concrete edit composes several accepted results, but each carrier
  // must independently discharge.
  for (size_t i = 0; i < edit.acceptedResults.size(); ++i) {
    const std::shared_ptr<const AcceptedResultCandidate> &carrier =
        edit.acceptedResults[i];
    if (!carrier) {
      const TerminalFallbackProofFailure failure =
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact,
              TerminalFallbackFailureContext::ForStateComponent(
                  "TextEdit.acceptedResults.null"));
      const bool strictRejected =
          theoremAuditService_.RejectNoLegacyAuditFindingIfStrict(
              RefoldTheoremAudit::MakeLegacyAuditEvidence(
                  LegacyPathKind::PathSpecificProofMirror, emissionStage,
                  llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has null "
                                "AcceptedResultCandidate carrier #{3}",
                                edit.start, edit.end, owner, i)
                      .str()),
              failure);
      theoremAuditService_.NoteTheoremAuditViolation(
          "emitted edit carried a null accepted-result carrier");
      if (!strictRejected) {
        terminalSink_.RequestTerminalFallback(
            failure, emissionStage,
            llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has null "
                          "accepted-result carrier #{3}",
                          edit.start, edit.end, owner, i)
                .str());
      }
      return false;
    }

    if (!carrierIsEmissionDischarged(*carrier)) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          emissionStage,
          llvm::formatv("emitted edit bytes=[{0},{1}) in {2} carries "
                        "non-discharged result #{3} kind={4}",
                        edit.start, edit.end, owner, i, carrier->kind)
              .str());
      return false;
    }
  }

  return EmittedTextEditHasOrderedAcceptedProofComposition(edit, emissionStage,
                                                           emissionOwner);
}

bool RefoldTextEditAssembler::EmittedTextEditHasOrderedAcceptedProofComposition(
    const TextEdit &edit, StringRef emissionStage,
    StringRef emissionOwner) const {
  if (edit.acceptedResults.size() <= 1)
    return true;

  ++theoremAudit_.emittedCompositeEdits;

  enum class CarrierSpanDomain : uint8_t { AToken, TUByte };
  struct CarrierSpan {
    CarrierSpanDomain domain;
    uint64_t begin = 0;
    uint64_t end = 0;
    size_t carrierIndex = 0;
    TheoremProofClass theoremProof = TheoremProofClass::Unknown;
  };

  auto domainName = [](CarrierSpanDomain domain) -> StringRef {
    switch (domain) {
    case CarrierSpanDomain::AToken:
      return "AToken";
    case CarrierSpanDomain::TUByte:
      return "TUByte";
    }
    return "Unknown";
  };

  const StringRef owner =
      emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner;

  auto failComposition = [&](StringRef detail) -> bool {
    ++theoremAudit_.emittedUncomposedCompositeEdits;
    theoremAuditService_.NoteTheoremAuditViolation(detail);
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionArtifactDischarged,
            TerminalFallbackFailureReason::UndischargedEmissionArtifact),
        emissionStage,
        llvm::formatv("emitted composite edit bytes=[{0},{1}) in {2}: {3}",
                      edit.start, edit.end, owner, detail)
            .str());
    return false;
  };

  auto extractSpan =
      [&](const AcceptedResultCandidate &carrier, size_t carrierIndex,
          TheoremProofClass theoremProof) -> std::optional<CarrierSpan> {
    CarrierSpan span;
    span.carrierIndex = carrierIndex;
    span.theoremProof = theoremProof;

    switch (carrier.kind) {
    case AcceptedResultCandidateKind::MacroPatch:
    case AcceptedResultCandidateKind::IncludePatch:
      // Macro and include carriers record A-side preprocessed-token envelopes.
      // They are comparable to one another for proof-composition purposes, but
      // not to the physical byte range of the final TextEdit.
      span.domain = CarrierSpanDomain::AToken;
      span.begin = carrier.begin;
      span.end = carrier.end;
      break;

    case AcceptedResultCandidateKind::TUAnchor:
      if (carrier.proofSummary.hasTUAnchorWitness &&
          carrier.proofSummary.tuAnchorWitness.hasPPGap) {
        span.domain = CarrierSpanDomain::AToken;
        span.begin = span.end = carrier.proofSummary.tuAnchorWitness.ppGap;
      } else if (carrier.hasAnchorByte) {
        span.domain = CarrierSpanDomain::TUByte;
        span.begin = span.end = carrier.anchorByte;
      } else {
        return std::nullopt;
      }
      break;

    case AcceptedResultCandidateKind::TUTextEdit:
      span.domain = CarrierSpanDomain::TUByte;
      span.begin = carrier.begin;
      span.end = carrier.end;
      break;

    case AcceptedResultCandidateKind::TerminalOutOfDomain:
    case AcceptedResultCandidateKind::Unknown:
      return std::nullopt;
    }

    if (span.end < span.begin)
      return std::nullopt;
    return span;
  };

  SmallVector<CarrierSpan, 8> spans;
  spans.reserve(edit.acceptedResults.size());
  for (size_t i = 0; i < edit.acceptedResults.size(); ++i) {
    const std::shared_ptr<const AcceptedResultCandidate> &carrierPtr =
        edit.acceptedResults[i];
    if (!carrierPtr)
      return failComposition("composite edit carried a null proof segment");

    const std::optional<TheoremProofClass> theoremProof =
        proofSummaryBuilder_.NormalizeAcceptedProof(*carrierPtr);
    if (!theoremProof)
      return failComposition(
          "composite edit contained a carrier that did not normalize");

    std::optional<CarrierSpan> span =
        extractSpan(*carrierPtr, i, *theoremProof);
    if (!span)
      return failComposition(
          llvm::formatv("composite carrier #{0} had no comparable proof span "
                        "kind={1}",
                        i, carrierPtr->kind)
              .str());
    spans.push_back(*span);
  }

  // Several construction paths can converge on the exact same concrete source
  // surface.  Those carriers are equivalent witnesses for one segment rather
  // than a sequence of multiple segments, so no ordering/gap proof is needed.
  bool sameSurface = true;
  for (size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].domain != spans[0].domain ||
        spans[i].begin != spans[0].begin || spans[i].end != spans[0].end) {
      sameSurface = false;
      break;
    }
  }
  if (sameSurface) {
    ++theoremAudit_.emittedEquivalentCompositeEdits;
    REFOLD_LOG_TRACE("proof/compose",
                     "composite edit bytes=[{0},{1}) in {2} has {3} equivalent "
                     "proof carriers over {4}[{5},{6})",
                     edit.start, edit.end, owner, spans.size(),
                     domainName(spans[0].domain), spans[0].begin, spans[0].end);
    return true;
  }

  // Duplicate physical edits can merge a TU-byte carrier with owner-local
  // A-token carriers.  When one normalized carrier proves the exact physical
  // emitted byte surface, the non-TU carriers are redundant witnesses rather
  // than additional ordered segments.  This preserves existing deterministic
  // duplicate-edit behavior without treating cross-domain carrier order as a
  // proof.
  for (const CarrierSpan &span : spans) {
    if (span.domain == CarrierSpanDomain::TUByte && span.begin == edit.start &&
        span.end == edit.end) {
      ++theoremAudit_.emittedEquivalentCompositeEdits;
      REFOLD_LOG_TRACE(
          "proof/compose",
          "composite edit bytes=[{0},{1}) in {2} is dominated by carrier "
          "#{3} theoremProof={4} over TUByte[{5},{6}); {7} auxiliary "
          "carriers remain individually discharged",
          edit.start, edit.end, owner, span.carrierIndex, span.theoremProof,
          span.begin, span.end, spans.size() - 1);
      return true;
    }
  }

  // Same-point insertion composition consumes no A-side bytes/tokens.  Multiple
  // zero-width carriers may therefore share one physical insertion edit without
  // needing an inter-segment source-gap proof; the emitted text order was fixed
  // by the duplicate-insertion merge that built this TextEdit.
  const bool allZeroWidth = llvm::all_of(
      spans, [](const CarrierSpan &span) { return span.begin == span.end; });
  if (allZeroWidth && edit.start == edit.end) {
    ++theoremAudit_.emittedEquivalentCompositeEdits;
    REFOLD_LOG_TRACE("proof/compose",
                     "composite insertion edit bytes=[{0},{1}) in {2} has {3} "
                     "zero-width proof carriers",
                     edit.start, edit.end, owner, spans.size());
    return true;
  }

  // Ordered segment composition is only meaningful inside one coordinate space.
  // Mixing A-token carriers with TU-byte carriers would require a separate
  // cross-domain owner-closure witness, so the proof model rejects that
  // composition instead of silently treating the carrier vector as an unordered
  // bag.
  for (size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].domain != spans[0].domain) {
      return failComposition(
          llvm::formatv(
              "carrier #{0} uses {1} coordinates after {2} coordinates; "
              "no cross-domain composition witness is attached",
              spans[i].carrierIndex, domainName(spans[i].domain),
              domainName(spans[0].domain))
              .str());
    }
  }

  // The carrier list itself must be ordered.  Sorting here would hide a missing
  // construction proof, so the audit checks the preserved order and rejects
  // overlaps or non-empty source gaps.  Future state-gap proof extensions can
  // relax the gap rule by attaching typed state-closed gap witnesses.
  for (size_t i = 1; i < spans.size(); ++i) {
    const CarrierSpan &prev = spans[i - 1];
    const CarrierSpan &cur = spans[i];
    if (cur.begin < prev.end) {
      return failComposition(
          llvm::formatv("carrier #{0} span {1}[{2},{3}) overlaps previous "
                        "carrier #{4} span {1}[{5},{6})",
                        cur.carrierIndex, domainName(cur.domain), cur.begin,
                        cur.end, prev.carrierIndex, prev.begin, prev.end)
              .str());
    }
    if (cur.begin > prev.end) {
      return failComposition(
          llvm::formatv(
              "carrier #{0} span {1}[{2},{3}) leaves non-empty "
              "unproved gap after carrier #{4} span {1}[{5},{6}); "
              "current composition proof admits only empty gaps until "
              "state-gap witnesses are available",
              cur.carrierIndex, domainName(cur.domain), cur.begin, cur.end,
              prev.carrierIndex, prev.begin, prev.end)
              .str());
    }
  }

  // If the carrier coordinates are physical TU bytes, they must tile the actual
  // source interval being emitted.  A-token carriers prove their own PP
  // envelope through the normalized per-carrier proof; the final TextEdit byte
  // span is a separately mapped owner surface and is not directly comparable
  // here.
  if (spans[0].domain == CarrierSpanDomain::TUByte &&
      (spans.front().begin != edit.start || spans.back().end != edit.end)) {
    return failComposition(
        llvm::formatv("TU-byte composite carriers tile [{0},{1}) but emitted "
                      "edit covers [{2},{3})",
                      spans.front().begin, spans.back().end, edit.start,
                      edit.end)
            .str());
  }

  ++theoremAudit_.emittedOrderedCompositeEdits;
  REFOLD_LOG_TRACE(
      "proof/compose",
      "composite edit bytes=[{0},{1}) in {2} has {3} ordered contiguous "
      "proof segments in {4} coordinates",
      edit.start, edit.end, owner, spans.size(), domainName(spans[0].domain));
  return true;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::TextEditMaterializedOutputTextRange(
    const TextEdit &edit) const {
  if (edit.materializedOutputTextBegin && edit.materializedOutputTextEnd) {
    if (*edit.materializedOutputTextEnd < *edit.materializedOutputTextBegin ||
        *edit.materializedOutputTextEnd >
            static_cast<uint64_t>(edit.text.size()))
      return std::nullopt;
    return std::make_pair(*edit.materializedOutputTextBegin,
                          *edit.materializedOutputTextEnd);
  }

  return std::make_pair(uint64_t{0}, static_cast<uint64_t>(edit.text.size()));
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::MacroPatchMaterializedOutputTextRange(
    const MacroPatch &patch) const {
  if (!patch.materialized.hasOutputByteRange)
    return std::nullopt;
  if (patch.materialized.outputByteEnd < patch.materialized.outputByteStart ||
      patch.materialized.outputByteEnd >
          static_cast<uint64_t>(patch.replacement.size()))
    return std::nullopt;
  return std::make_pair(patch.materialized.outputByteStart,
                        patch.materialized.outputByteEnd);
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::TextEditMaterializedBByteRange(
    const TextEdit &edit) const {
  // A certified B-payload-free edit has no envelope by construction. Callers
  // must consult `materializesNoBPayload` before treating this as the missing
  // certificate that fails closed.
  if (edit.materializesNoBPayload)
    return std::nullopt;

  if (edit.materializedBByteBegin && edit.materializedBByteEnd) {
    if (*edit.materializedBByteEnd < *edit.materializedBByteBegin ||
        *edit.materializedBByteEnd > static_cast<uint64_t>(bSource_.size()))
      return std::nullopt;
    return std::make_pair(*edit.materializedBByteBegin,
                          *edit.materializedBByteEnd);
  }

  if (edit.directTUHunkBStart && edit.directTUHunkBEnd)
    return sourceMapper_.BTokenRangeToByteRange(*edit.directTUHunkBStart,
                                                *edit.directTUHunkBEnd);

  return std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldTextEditAssembler::MacroPatchMaterializedBByteRange(
    const MacroPatch &patch) const {
  // Prefer an explicitly certified envelope.  Structure-preserving macro
  // patches use this to distinguish two edit-map meanings that share the same
  // physical TextEdit: ordinary argument rewrites map the B side to the whole
  // expansion envelope they regenerate, while pure insertions intentionally
  // stay narrow and let the final TextEdit/hunk metadata provide the inserted
  // payload.
  if (patch.materialized.hasBTokenRange) {
    if (auto bytes = sourceMapper_.BTokenRangeToByteRange(
            patch.materialized.bTokStart, patch.materialized.bTokEnd))
      return bytes;
  }

  uint64_t macroId = patch.proof.proofRootMacroId ? patch.proof.proofRootMacroId
                                                  : patch.macroId;
  if (macroId == 0)
    return std::nullopt;

  const RefoldModel::MacroInvocation *macro =
      macroTopology_.FindMacroInvocationById(macroId);
  if (!macro)
    return std::nullopt;

  if (auto plan = wholeCoverPlanBuilder_.ComputeWholeCoverPlan(*macro))
    return sourceMapper_.BTokenRangeToByteRange(plan->bTokStart, plan->bTokEnd);

  if (auto env =
          sourceMapper_.MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
              macro->cover.begin, macro->cover.end))
    return sourceMapper_.BTokenRangeToByteRange(env->first, env->second);

  return std::nullopt;
}

std::string RefoldTextEditAssembler::ApplyTextEditsWithPendingResync(
    StringRef originalFileText, ArrayRef<TextEdit> edits,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds, StringRef emissionOwner,
    std::optional<uint64_t> ownerIncludeId,
    std::vector<MaterializedEditMapping> *materializedEditMappings,
    std::vector<FinalLineControlPruneCandidate> *lineControlPruneCandidates,
    std::vector<FinalLineControlSourceMapping> *lineControlSourceMappings)
    const {
  // Source-side ranges are only knowable at this final splice boundary: every
  // earlier candidate still may be merged, dropped, widened, or followed by a
  // pending resync before it becomes physical output.
  if (materializedEditMappings)
    materializedEditMappings->clear();
  if (lineControlPruneCandidates)
    lineControlPruneCandidates->clear();
  if (lineControlSourceMappings)
    lineControlSourceMappings->clear();
  const std::string sourceMappingOwner =
      emissionOwner.empty() ? std::string()
                            : lineDirs_.ToAbsolutePath(emissionOwner);
  if (edits.empty()) {
    if (lineControlSourceMappings && !originalFileText.empty() &&
        !sourceMappingOwner.empty()) {
      lineControlSourceMappings->push_back(FinalLineControlSourceMapping{
          0, static_cast<uint64_t>(originalFileText.size()), sourceMappingOwner,
          0, static_cast<uint64_t>(originalFileText.size()), ownerIncludeId});
    }
    return originalFileText.str();
  }

  // Normalize edits by span.
  //
  // Multiple insertions may legitimately share the same zero-length span.  This
  // happens when the diff produces adjacent insert-only hunks (for example when
  // comments tokenize separately), and both map to the same insertion point.
  //
  // The assembler:
  //  * preserves and concatenates multiple INSERT edits with identical
  //    zero-length spans ([x,x)) in their original order.
  //  * merges duplicate non-zero edits only when they carry the same
  //    replacement payload; conflicting duplicate replacements request the
  //    explicit terminal fallback instead of selecting a writer by order.

  struct EditRef {
    const TextEdit *e;
    size_t idx;
  };

  SmallVector<EditRef, 32> ordered;
  ordered.reserve(edits.size());
  for (size_t i = 0; i < edits.size(); ++i)
    ordered.push_back(EditRef{&edits[i], i});

  sort(ordered, [](const EditRef &a, const EditRef &b) {
    if (a.e->start != b.e->start)
      return a.e->start < b.e->start;
    if (a.e->end != b.e->end)
      return a.e->end < b.e->end;
    return a.idx < b.idx;
  });

  SmallVector<TextEdit, 32> norm;
  norm.reserve(ordered.size());

  for (size_t i = 0; i < ordered.size();) {
    const uint64_t s = ordered[i].e->start;
    const uint64_t t = ordered[i].e->end;

    size_t j = i + 1;
    while (j < ordered.size() && ordered[j].e->start == s &&
           ordered[j].e->end == t)
      ++j;

    if (j - i == 1) {
      norm.push_back(*ordered[i].e);
      i = j;
      continue;
    }

    // Multiple edits share the same byte span.
    if (s == t) {
      // INSERT at identical position: concatenate in order.
      TextEdit merged;
      merged.start = s;
      merged.end = t;

      size_t totalLen = 0;
      for (size_t k = i; k < j; ++k)
        totalLen += ordered[k].e->text.size();
      merged.text.reserve(totalLen);

      std::optional<uint64_t> mergedBByteBegin;
      std::optional<uint64_t> mergedBByteEnd;
      bool mergedBByteRangeContiguous = true;
      std::optional<uint64_t> mergedOutByteBegin;
      std::optional<uint64_t> mergedOutByteEnd;
      // A fragment certified to realize no B bytes tiles the empty B range, so
      // it can neither break the contiguity of the surrounding witnesses nor
      // widen the merged refolded-output envelope. The merged edit inherits the
      // certificate only when every fragment carried it.
      bool mergedMaterializesNoBPayload = true;
      // Same-offset insertions become one physical edit in source order. The
      // sidecar can describe that merged edit only when the contributing
      // B-side witnesses tile one contiguous byte range in the same order.
      for (size_t k = i; k < j; ++k) {
        const uint64_t fragmentOutputBase =
            static_cast<uint64_t>(merged.text.size());
        merged.text.append(ordered[k].e->text);
        appendShiftedLineControlPruneCandidates(
            merged.lineControlPruneCandidates,
            ordered[k].e->lineControlPruneCandidates, fragmentOutputBase);
        appendShiftedLineControlSourceMappings(
            merged.lineControlSourceMappings,
            ordered[k].e->lineControlSourceMappings, fragmentOutputBase);
        if (ordered[k].e->pending)
          merged.pending = ordered[k].e->pending;
        merged.acceptedResults.insert(merged.acceptedResults.end(),
                                      ordered[k].e->acceptedResults.begin(),
                                      ordered[k].e->acceptedResults.end());
        appendUniqueProtectedSourceAuthorizations(merged, *ordered[k].e);
        if (ordered[k].e->materializesNoBPayload)
          continue;

        mergedMaterializesNoBPayload = false;
        if (auto bRange = TextEditMaterializedBByteRange(*ordered[k].e)) {
          if (!mergedBByteBegin) {
            mergedBByteBegin = bRange->first;
            mergedBByteEnd = bRange->second;
          } else if (mergedBByteEnd && *mergedBByteEnd == bRange->first) {
            mergedBByteEnd = bRange->second;
          } else {
            mergedBByteRangeContiguous = false;
          }
        } else if (materializedEditMappings) {
          mergedBByteRangeContiguous = false;
        }
        if (auto outRange =
                TextEditMaterializedOutputTextRange(*ordered[k].e)) {
          const uint64_t outBegin = fragmentOutputBase + outRange->first;
          const uint64_t outEnd = fragmentOutputBase + outRange->second;
          mergedOutByteBegin = mergedOutByteBegin
                                   ? std::min(*mergedOutByteBegin, outBegin)
                                   : outBegin;
          mergedOutByteEnd =
              mergedOutByteEnd ? std::max(*mergedOutByteEnd, outEnd) : outEnd;
        }
      }
      if (mergedMaterializesNoBPayload)
        textEditCertifier_.CertifyTextEditMaterializesNoBPayload(merged);
      else if (mergedBByteBegin && mergedBByteEnd && mergedBByteRangeContiguous)
        textEditCertifier_.CertifyTextEditMaterializedBByteRange(
            merged, *mergedBByteBegin, *mergedBByteEnd);
      if (mergedOutByteBegin && mergedOutByteEnd)
        textEditCertifier_.CertifyTextEditMaterializedOutputTextRange(
            merged, *mergedOutByteBegin, *mergedOutByteEnd);

      deduplicateLineControlPruneCandidates(merged.lineControlPruneCandidates);
      deduplicateLineControlSourceMappings(merged.lineControlSourceMappings);

      norm.push_back(std::move(merged));
      i = j;
      continue;
    }

    // Non-zero span duplicates must be identical to compose safely. Choosing
    // an arbitrary writer for the same replaced byte range is not a proof; if
    // two producers disagree about the replacement payload, fail closed and let
    // the caller select the explicit terminal fallback path.
    const TextEdit *first = ordered[i].e;
    bool payloadsAgree = true;
    for (size_t k = i + 1; k < j; ++k) {
      if (ordered[k].e->text != first->text) {
        payloadsAgree = false;
        break;
      }
    }

    if (!payloadsAgree) {
      auto tryMergeDirectDuplicateFragments = [&]() -> std::optional<TextEdit> {
        SmallVector<const TextEdit *, 8> fragments;
        fragments.reserve(j - i);

        // This merge is only valid for duplicate fragments that still have a
        // direct one-to-one TU hunk witness. Anything synthesized, macro-owned,
        // include-owned, or missing hunk provenance must stay on the normal
        // composition path.
        for (size_t k = i; k < j; ++k) {
          const TextEdit &edit = *ordered[k].e;
          if (!edit.isDirectTUHunkEdit || !edit.directTUHunkIndex ||
              !edit.directTUHunkAStart || !edit.directTUHunkAEnd ||
              !edit.directTUHunkBStart || !edit.directTUHunkBEnd)
            return std::nullopt;
          fragments.push_back(&edit);
        }

        // Reconstruct the original hunk order before proving adjacency. The
        // duplicate group is ordered by emitted byte span, which is not enough
        // to prove that the B-side replacement interval is contiguous.
        sort(fragments, [](const TextEdit *lhs, const TextEdit *rhs) {
          return *lhs->directTUHunkIndex < *rhs->directTUHunkIndex;
        });

        // Adjacent duplicate fragments may be collapsed only when they form one
        // uninterrupted run in both the A token hunks and B token hunks. This
        // prevents merging unrelated fragments that merely overlap the same
        // emitted byte span.
        for (size_t k = 1; k < fragments.size(); ++k) {
          const TextEdit &prev = *fragments[k - 1];
          const TextEdit &cur = *fragments[k];

          if (*cur.directTUHunkIndex != *prev.directTUHunkIndex + 1)
            return std::nullopt;
          if (*cur.directTUHunkAStart != *prev.directTUHunkAEnd)
            return std::nullopt;
          if (*cur.directTUHunkBStart != *prev.directTUHunkBEnd)
            return std::nullopt;
        }

        // Re-prove the complete A-token run before composing the fragments.
        // Individual PlanTUByteSpan() witnesses do not establish that bytes
        // between adjacent hunks are trivia; the combined query discharges the
        // exact-mapping, ordering, overlap, and internal-gap obligations for the
        // complete token envelope.  The final emitted range may be wider only
        // because a constituent carried an independently proved boundary
        // extension, so it must contain the combined base span.
        std::optional<TUByteSpanPlan> combinedBaseSpan =
            tuAnchorProof_.PlanTUByteSpan(
                *fragments.front()->directTUHunkAStart,
                *fragments.back()->directTUHunkAEnd, emissionOwner);
        if (!combinedBaseSpan || s > combinedBaseSpan->tuByteBegin ||
            combinedBaseSpan->tuByteEnd > t) {
          return std::nullopt;
        }

        // Merging still recreates one larger physical source edit. Re-run the
        // preprocessing-state firewall over that actual envelope. Complete
        // producer-bound macro transitions may remain only as provisional
        // evidence for the later repair planner; this merge creates no source
        // authority and may absorb no other directive or pragma operator.
        if (!tuAnchorProof_.ValidateDirectTUEnvelope(emissionOwner, s, t)) {
          return std::nullopt;
        }

        // The merged replacement is exactly the contiguous B-token envelope
        // covered by the duplicate fragment run.
        const uint64_t bBegin = *fragments.front()->directTUHunkBStart;
        const uint64_t bEnd = *fragments.back()->directTUHunkBEnd;
        if (bBegin >= bEnd || bEnd > static_cast<uint64_t>(bToks_.size()))
          return std::nullopt;

        StringRef replacement = refoldSliceExactTokenCoverage(
            bTokOff_, bToks_, bSource_, bBegin, bEnd);
        if (replacement.empty())
          return std::nullopt;

        // Apply normal resync handling to the whole duplicate byte span, rather
        // than preserving each fragment's already-conflicting local resync.
        ResyncOutcome ro = lineObserverLayout_.ApplyResyncOrPend(
            originalFileText, s, t, replacement, emissionOwner, ownerIncludeId);
        TextEdit merged{
            s,  t, std::move(ro.text), std::move(ro.pending), std::nullopt, {},
            {}, {}};
        merged.lineControlPruneCandidates =
            std::move(ro.lineControlPruneCandidates);
        textEditCertifier_.CertifyTextEditMaterializedBTokenRange(merged,
                                                                  bBegin, bEnd);

        // Preserve existing proof carriers from the fragments. If the fragments
        // carry macro-root provenance, they must all agree on the same root.
        for (const TextEdit *fragment : fragments) {
          if (fragment->expandedMacroRootId) {
            if (merged.expandedMacroRootId &&
                *merged.expandedMacroRootId != *fragment->expandedMacroRootId)
              return std::nullopt;
            merged.expandedMacroRootId = fragment->expandedMacroRootId;
          }
          merged.acceptedResults.insert(merged.acceptedResults.end(),
                                        fragment->acceptedResults.begin(),
                                        fragment->acceptedResults.end());
          appendUniqueProtectedSourceAuthorizations(merged, *fragment);
        }

        // Add a carrier for the actual emitted merged surface so the final
        // proof gate can reason about this replacement as one conservative TU
        // edit. The synthetic hunk is not manufactured proof: its A/B envelopes
        // are the already-validated contiguous fragment union, and the final
        // span retains only specialized capabilities already discharged by the
        // absorbed fragments; direct span planning contributes none.
        const diffutils::Hunk mergedHunk{
            *fragments.front()->directTUHunkAStart,
            *fragments.back()->directTUHunkAEnd, bBegin, bEnd};
        TUByteSpanPlan mergedSpan(mergedHunk.aStart, mergedHunk.aEnd, s,
                                  t, combinedBaseSpan->insertionAnchor);
        textEditCertifier_.AttachAcceptedResultCarrier(
            merged, acceptedCandidateBuilder_.BuildAcceptedTUTextEditCandidate(
                        AcceptedPathKind::TUByteSpanConservativeEdit,
                        mergedHunk, mergedSpan,
                        /*structuralBinding=*/nullptr, replacement));

        return merged;
      };

      // Split token-LCS frontiers can produce several direct TU hunk fragments
      // for the same original byte span. If those fragments are adjacent in
      // both A-token and B-token space, they are not competing writers: they
      // are one replacement that was fragmented before byte emission. Rebuild
      // that replacement from the closed B-token interval; otherwise keep the
      // existing fail-closed duplicate-edit policy.
      if (std::optional<TextEdit> mergedDirectDuplicate =
              tryMergeDirectDuplicateFragments()) {
        norm.push_back(std::move(*mergedDirectDuplicate));
        i = j;
        continue;
      }

      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/apply",
          llvm::formatv(
              "conflicting duplicate non-zero edits for span [{0},{1}) in {2}",
              s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }

    TextEdit merged = *first;
    std::optional<std::pair<uint64_t, uint64_t>> mergedBRange =
        TextEditMaterializedBByteRange(merged);
    // Duplicate writers of one physical replacement must agree on whether that
    // replacement realizes B payload at all. One duplicate claiming a B-side
    // envelope while another certifies the payload-free case is exactly the
    // ambiguity this merge is not allowed to resolve.
    bool mergedBRangeAmbiguous = false;
    std::optional<std::pair<uint64_t, uint64_t>> mergedOutRange =
        TextEditMaterializedOutputTextRange(merged);
    bool mergedOutRangeAmbiguous = false;
    for (size_t k = i + 1; k < j; ++k) {
      const TextEdit &dup = *ordered[k].e;

      // Identical non-zero edits collapse to one physical replacement, but any
      // deferred line-resync state produced by the later duplicate must still
      // be preserved on the merged edit.
      if (dup.pending)
        merged.pending = dup.pending;

      // If duplicate edits represent an expanded macro replacement, they must
      // agree on the same expanded root. Different roots for the same concrete
      // byte span would make the emitted edit's proof carrier ambiguous.
      if (merged.expandedMacroRootId && dup.expandedMacroRootId &&
          *merged.expandedMacroRootId != *dup.expandedMacroRootId) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::EmissionEditSetComposable,
                TerminalFallbackFailureReason::UncomposableEmissionEditSet),
            "edits/apply",
            llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                          "carry incompatible expanded macro roots {3} and {4}",
                          s, t, emissionOwner, *merged.expandedMacroRootId,
                          *dup.expandedMacroRootId)
                .str());
        return originalFileText.str();
      }

      // A duplicate may carry the expanded-root marker even when the first edit
      // did not. Preserve that marker so counter stabilization and audit still
      // see the emitted replacement as macro-expanded.
      if (!merged.expandedMacroRootId && dup.expandedMacroRootId)
        merged.expandedMacroRootId = dup.expandedMacroRootId;

      if (dup.materializesNoBPayload != merged.materializesNoBPayload)
        mergedBRangeAmbiguous = true;

      if (std::optional<std::pair<uint64_t, uint64_t>> dupBRange =
              TextEditMaterializedBByteRange(dup)) {
        if (!mergedBRange) {
          mergedBRange = dupBRange;
        } else if (mergedBRange->first != dupBRange->first ||
                   mergedBRange->second != dupBRange->second) {
          mergedBRangeAmbiguous = true;
        }
      }

      if (std::optional<std::pair<uint64_t, uint64_t>> dupOutRange =
              TextEditMaterializedOutputTextRange(dup)) {
        if (!mergedOutRange) {
          mergedOutRange = dupOutRange;
        } else if (mergedOutRange->first != dupOutRange->first ||
                   mergedOutRange->second != dupOutRange->second) {
          mergedOutRangeAmbiguous = true;
        }
      }

      // Keep all accepted-result carriers from the equivalent duplicates. The
      // text replacement is shared, but each proof witness still explains one
      // path that contributed to the emitted edit.
      appendShiftedLineControlPruneCandidates(merged.lineControlPruneCandidates,
                                              dup.lineControlPruneCandidates,
                                              /*delta=*/0);
      appendShiftedLineControlSourceMappings(merged.lineControlSourceMappings,
                                             dup.lineControlSourceMappings,
                                             /*delta=*/0);
      merged.acceptedResults.insert(merged.acceptedResults.end(),
                                    dup.acceptedResults.begin(),
                                    dup.acceptedResults.end());
      appendUniqueProtectedSourceAuthorizations(merged, dup);
    }

    if (mergedBRangeAmbiguous && materializedEditMappings) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          "edit-map",
          llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                        "carry different B-side materialization ranges",
                        s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }
    if (mergedOutRangeAmbiguous && materializedEditMappings) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          "edit-map",
          llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                        "carry different refolded-output materialization "
                        "ranges",
                        s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }
    if (mergedBRange && !merged.materializesNoBPayload)
      textEditCertifier_.CertifyTextEditMaterializedBByteRange(
          merged, mergedBRange->first, mergedBRange->second);
    if (mergedOutRange)
      textEditCertifier_.CertifyTextEditMaterializedOutputTextRange(
          merged, mergedOutRange->first, mergedOutRange->second);

    deduplicateLineControlPruneCandidates(merged.lineControlPruneCandidates);
    deduplicateLineControlSourceMappings(merged.lineControlSourceMappings);

    norm.push_back(std::move(merged));
    i = j;
  }

  auto sortNormalizedTextEdits = [](SmallVectorImpl<TextEdit> &items) {
    sort(items, [](const TextEdit &a, const TextEdit &b) {
      if (a.start != b.start)
        return a.start < b.start;
      return a.end < b.end;
    });
  };

  auto findDirectEditForHunk =
      [&](uint64_t hunkIndex) -> std::optional<size_t> {
    for (size_t idx = 0; idx < norm.size(); ++idx) {
      const TextEdit &edit = norm[idx];
      if (edit.directTUHunkIndex && *edit.directTUHunkIndex == hunkIndex)
        return idx;
    }
    return std::nullopt;
  };

  auto sourceTokenSpan =
      [&](uint64_t aTok) -> std::optional<std::pair<uint64_t, uint64_t>> {
    if (aTok >= static_cast<uint64_t>(aToks_.size()))
      return std::nullopt;
    if (auto span =
            tuAnchorProof_.PlanTUByteSpan(aTok, aTok + 1, emissionOwner))
      return span->byteRange();
    return std::nullopt;
  };

  struct ClosedTURealizationCandidate {
    uint64_t firstHunk = 0;
    uint64_t lastHunk = 0;
    uint64_t sourceBegin = 0;
    uint64_t sourceEnd = 0;
    uint64_t aBegin = 0;
    uint64_t aEnd = 0;
    uint64_t bBegin = 0;
    uint64_t bEnd = 0;
    std::optional<TUInsertionAnchor> insertionAnchor;
    SmallVector<size_t, 16> editIndices;
  };

  // Validate that a contiguous run of direct TU-hunk edits can be replaced by
  // one closed TU realization. The candidate is accepted only when every hunk
  // in the run has a direct edit, the source/B envelopes are monotone and
  // non-empty, no independent edit intrudes into the source interval, and the
  // A↔B token map proves that all stable tokens crossing either envelope stay
  // inside the corresponding opposite envelope.
  auto checkClosedTURealizationCandidate =
      [&](uint64_t firstHunk, uint64_t lastHunk,
          ClosedTURealizationCandidate &candidate) -> bool {
    candidate = ClosedTURealizationCandidate{};
    candidate.firstHunk = firstHunk;
    candidate.lastHunk = lastHunk;

    // The closure is defined over a contiguous hunk run, so every hunk in the
    // run must already have a direct TU edit that can be absorbed.
    for (uint64_t hunkIndex = firstHunk; hunkIndex <= lastHunk; ++hunkIndex) {
      std::optional<size_t> editIndex = findDirectEditForHunk(hunkIndex);
      if (!editIndex)
        return false;
      candidate.editIndices.push_back(*editIndex);
      if (hunkIndex == std::numeric_limits<uint64_t>::max())
        break;
    }

    candidate.sourceBegin = std::numeric_limits<uint64_t>::max();
    candidate.sourceEnd = 0;
    candidate.bBegin = std::numeric_limits<uint64_t>::max();
    candidate.bEnd = 0;

    // Build the source byte envelope and B-token envelope for the selected
    // direct edits. The B intervals must appear in order; otherwise a single
    // replacement would not preserve the token-order proof for the hunk run.
    std::optional<uint64_t> previousBEnd;
    for (size_t editIndex : candidate.editIndices) {
      const TextEdit &edit = norm[editIndex];
      if (!edit.isDirectTUHunkEdit || !edit.directTUHunkBStart ||
          !edit.directTUHunkBEnd)
        return false;
      if (previousBEnd && *edit.directTUHunkBStart < *previousBEnd)
        return false;
      previousBEnd = edit.directTUHunkBEnd;
      candidate.sourceBegin = std::min(candidate.sourceBegin, edit.start);
      candidate.sourceEnd = std::max(candidate.sourceEnd, edit.end);
      candidate.bBegin = std::min(candidate.bBegin, *edit.directTUHunkBStart);
      candidate.bEnd = std::max(candidate.bEnd, *edit.directTUHunkBEnd);
    }

    // The synthesized replacement must cover a real source byte interval and a
    // real B-token interval.
    if (candidate.sourceBegin == std::numeric_limits<uint64_t>::max() ||
        candidate.sourceBegin >= candidate.sourceEnd)
      return false;
    if (candidate.bBegin == std::numeric_limits<uint64_t>::max() ||
        candidate.bBegin >= candidate.bEnd ||
        candidate.bEnd > static_cast<uint64_t>(bToks_.size()))
      return false;

    // Re-run PlanTUByteSpan() over the complete A-token run represented by
    // the synthesized candidate.  This is the shared proof boundary for exact
    // mappings, source monotonicity/nonoverlap, and authorized internal gaps;
    // checking only the already-planned fragments would leave the bytes between
    // them unaudited.  Independently proved boundary extensions may make the
    // actual source envelope wider, but it must contain the combined base span.
    const diffutils::Hunk &firstHunkRecord =
        abTokHunks_[static_cast<size_t>(firstHunk)];
    const diffutils::Hunk &lastHunkRecord =
        abTokHunks_[static_cast<size_t>(lastHunk)];
    std::optional<TUByteSpanPlan> combinedBaseSpan =
        tuAnchorProof_.PlanTUByteSpan(firstHunkRecord.aStart,
                                      lastHunkRecord.aEnd, emissionOwner);
    if (!combinedBaseSpan ||
        candidate.sourceBegin > combinedBaseSpan->tuByteBegin ||
        combinedBaseSpan->tuByteEnd > candidate.sourceEnd) {
      return false;
    }
    candidate.aBegin = firstHunkRecord.aStart;
    candidate.aEnd = lastHunkRecord.aEnd;
    candidate.insertionAnchor = combinedBaseSpan->insertionAnchor;

    // This path deliberately synthesizes a wider direct TU edit from several
    // already-planned fragments. Its ordinary-token closure checks below do
    // not authorize preprocessing state. Validate the actual envelope so it
    // contains at most complete producer-bound macro-transition evidence for
    // the later repair planner; any additional or unbound structure rejects
    // the candidate.
    if (!tuAnchorProof_.ValidateDirectTUEnvelope(
            emissionOwner, candidate.sourceBegin, candidate.sourceEnd)) {
      return false;
    }

    auto candidateContainsEdit = [&](size_t editIndex) {
      for (size_t candidateEditIndex : candidate.editIndices) {
        if (candidateEditIndex == editIndex)
          return true;
      }
      return false;
    };

    // The closure may compose only the selected hunk-run edits. Any other edit
    // that overlaps the source envelope, including a zero-width insertion
    // inside it, would need a separate composition law and is rejected here.
    for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
      if (candidateContainsEdit(editIndex))
        continue;
      const TextEdit &edit = norm[editIndex];
      const bool nonZeroOverlap =
          edit.start < candidate.sourceEnd && candidate.sourceBegin < edit.end;
      const bool interiorInsertion = edit.start == edit.end &&
                                     candidate.sourceBegin < edit.start &&
                                     edit.start < candidate.sourceEnd;
      if (nonZeroOverlap || interiorInsertion)
        return false;
    }

    // A-side closure: every A token whose source bytes overlap the candidate
    // must be wholly contained by the candidate source interval. If that token
    // has a stable B mate, that B mate must also lie inside the candidate
    // replacement envelope.
    for (uint64_t aTok = 0; aTok < static_cast<uint64_t>(aToks_.size());
         ++aTok) {
      std::optional<std::pair<uint64_t, uint64_t>> span = sourceTokenSpan(aTok);
      if (!span)
        continue;

      const bool overlapsSource = span->first < candidate.sourceEnd &&
                                  candidate.sourceBegin < span->second;
      if (!overlapsSource)
        continue;

      if (span->first < candidate.sourceBegin ||
          candidate.sourceEnd < span->second)
        return false;

      if (aTok < static_cast<uint64_t>(abTokMapA2B_.size())) {
        const int64_t mappedB = abTokMapA2B_[static_cast<size_t>(aTok)];
        if (mappedB >= 0 &&
            (static_cast<uint64_t>(mappedB) < candidate.bBegin ||
             static_cast<uint64_t>(mappedB) >= candidate.bEnd))
          return false;
      }
    }

    // B-side closure: every stable B token emitted by the replacement must map
    // back to an A token wholly contained by the source interval. This prevents
    // the synthesized edit from duplicating or moving stable outside tokens.
    for (uint64_t bTok = candidate.bBegin; bTok < candidate.bEnd; ++bTok) {
      if (bTok >= static_cast<uint64_t>(abTokMapB2A_.size()))
        return false;
      const int64_t mappedA = abTokMapB2A_[static_cast<size_t>(bTok)];
      if (mappedA < 0)
        continue;

      std::optional<std::pair<uint64_t, uint64_t>> span =
          sourceTokenSpan(static_cast<uint64_t>(mappedA));
      if (!span || span->first < candidate.sourceBegin ||
          candidate.sourceEnd < span->second)
        return false;
    }

    return true;
  };

  // Try to replace an uncomposable cluster of overlapping direct TU-hunk edits
  // with one conservative closed TU realization. The synthesized edit is
  // accepted only if a contiguous direct-hunk run can be proven closed in both
  // source-byte space and B-token space by the A↔B token maps.
  auto tryBuildClosedTURealization = [&](size_t overlapIndex,
                                         TextEdit &closure) -> bool {
    // Closed realization depends on both token-map directions: A tokens inside
    // the source interval must map into the B envelope, and stable B tokens in
    // the B envelope must map back into the source interval.
    if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
      return false;

    // Start from the overlap point and collect the full source-overlap cluster
    // around it. The cluster determines the minimum set of direct edits that
    // must be absorbed by the synthesized closure.
    size_t clusterBegin = overlapIndex == 0 ? 0 : overlapIndex - 1;
    uint64_t coverBegin = norm[clusterBegin].start;
    uint64_t coverEnd = norm[clusterBegin].end;
    while (clusterBegin > 0) {
      const TextEdit &prev = norm[clusterBegin - 1];
      if (prev.end <= coverBegin)
        break;
      --clusterBegin;
      coverBegin = std::min(coverBegin, prev.start);
      coverEnd = std::max(coverEnd, prev.end);
    }

    size_t clusterEnd = overlapIndex + 1;
    if (overlapIndex < norm.size()) {
      coverBegin = std::min(coverBegin, norm[overlapIndex].start);
      coverEnd = std::max(coverEnd, norm[overlapIndex].end);
    }
    while (clusterEnd < norm.size()) {
      const TextEdit &next = norm[clusterEnd];
      if (next.start >= coverEnd)
        break;
      coverBegin = std::min(coverBegin, next.start);
      coverEnd = std::max(coverEnd, next.end);
      ++clusterEnd;
    }

    // Every edit in the overlap cluster must be a direct TU-hunk edit. Mixed
    // edit classes would require a separate composition law, so fail closed.
    std::optional<uint64_t> hunkMin;
    std::optional<uint64_t> hunkMax;
    for (size_t i = clusterBegin; i < clusterEnd; ++i) {
      const TextEdit &edit = norm[i];
      if (!edit.isDirectTUHunkEdit || !edit.directTUHunkIndex)
        return false;
      hunkMin = hunkMin ? std::min(*hunkMin, *edit.directTUHunkIndex)
                        : *edit.directTUHunkIndex;
      hunkMax = hunkMax ? std::max(*hunkMax, *edit.directTUHunkIndex)
                        : *edit.directTUHunkIndex;
    }
    if (!hunkMin || !hunkMax)
      return false;

    // Expand to the maximal contiguous run of direct TU hunks available around
    // the cluster. The search below starts with the minimal cluster-covering
    // run and widens only as needed to find a closed realization.
    uint64_t directLo = *hunkMin;
    while (directLo > 0 && findDirectEditForHunk(directLo - 1))
      --directLo;

    uint64_t directHi = *hunkMax;
    while (directHi + 1 < static_cast<uint64_t>(abTokHunks_.size()) &&
           findDirectEditForHunk(directHi + 1))
      ++directHi;

    // Prefer the smallest closed hunk run that contains the overlap cluster.
    // This keeps the synthesized TU edit as local as possible while still
    // allowing widening when closure requires adjacent direct hunks.
    std::optional<ClosedTURealizationCandidate> best;
    const uint64_t minWidth = *hunkMax - *hunkMin + 1;
    const uint64_t maxWidth = directHi - directLo + 1;
    for (uint64_t width = minWidth; width <= maxWidth; ++width) {
      bool searchedAnyAtThisWidth = false;
      for (uint64_t first = directLo; first + width - 1 <= directHi; ++first) {
        const uint64_t last = first + width - 1;
        if (first > *hunkMin || last < *hunkMax)
          continue;
        searchedAnyAtThisWidth = true;

        ClosedTURealizationCandidate candidate;
        if (checkClosedTURealizationCandidate(first, last, candidate)) {
          best = std::move(candidate);
          break;
        }
      }
      if (best || !searchedAnyAtThisWidth)
        break;
    }
    if (!best)
      return false;

    // Materialize exactly the proven B-token envelope, then apply the same
    // line-resync machinery used by ordinary emitted edits.
    StringRef replacement = refoldSliceExactTokenCoverage(
        bTokOff_, bToks_, bSource_, best->bBegin, best->bEnd);
    ResyncOutcome ro = lineObserverLayout_.ApplyResyncOrPend(
        originalFileText, best->sourceBegin, best->sourceEnd, replacement,
        emissionOwner, ownerIncludeId);
    closure = TextEdit{best->sourceBegin,
                       best->sourceEnd,
                       std::move(ro.text),
                       std::move(ro.pending),
                       std::nullopt,
                       {},
                       {},
                       {}};
    closure.lineControlPruneCandidates =
        std::move(ro.lineControlPruneCandidates);
    textEditCertifier_.CertifyTextEditMaterializedBTokenRange(
        closure, best->bBegin, best->bEnd);
    const diffutils::Hunk closedHunk{best->aBegin, best->aEnd, best->bBegin,
                                      best->bEnd};
    TUByteSpanPlan closedSpan(best->aBegin, best->aEnd,
                              best->sourceBegin, best->sourceEnd,
                              best->insertionAnchor);
    for (size_t editIndex : best->editIndices)
      appendUniqueProtectedSourceAuthorizations(closure, norm[editIndex]);
    textEditCertifier_.AttachAcceptedResultCarrier(
        closure, acceptedCandidateBuilder_.BuildAcceptedTUTextEditCandidate(
                     AcceptedPathKind::TUByteSpanConservativeEdit, closedHunk,
                     closedSpan,
                     /*structuralBinding=*/nullptr, replacement));

    // Replace the absorbed direct edits with the single closed realization and
    // re-sort so downstream application sees a normal non-overlapping edit set.
    SmallVector<TextEdit, 32> resolved;
    resolved.reserve(norm.size() - best->editIndices.size() + 1);
    auto shouldRemove = [&](size_t editIndex) {
      for (size_t candidateEditIndex : best->editIndices) {
        if (candidateEditIndex == editIndex)
          return true;
      }
      return false;
    };
    for (size_t i = 0; i < norm.size(); ++i) {
      if (!shouldRemove(i))
        resolved.push_back(std::move(norm[i]));
    }
    resolved.push_back(std::move(closure));
    sortNormalizedTextEdits(resolved);
    norm = std::move(resolved);

    return true;
  };

  // Remove edits the emitted bytes cannot contain before looking for an
  // overlap.  A subsumed edit is not an overlap to be resolved: its bytes are
  // replaced wholesale by the edit containing it, so it has no effect on the
  // output and no composition question to answer.
  dropSubsumedNormalizedTextEdits(norm, emissionOwner, lexLang_);

  // Some token-LCS tie choices can split one logical B-side TU realization
  // around stable punctuation tokens. If the resulting direct TU hunk edits
  // overlap after lexical widening, first try to replace the minimal enclosing
  // direct-hunk run by one source/B-token-closed realization. This is not a
  // merge heuristic: it is accepted only when the A→B and B→A token maps prove
  // that the source interval and replacement interval are mutually closed.
  for (;;) {
    uint64_t cursor = 0;
    bool changed = false;
    for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
      const TextEdit &edit = norm[editIndex];
      if (edit.end < edit.start || edit.end > originalFileText.size())
        break;
      if (edit.start < cursor) {
        TextEdit closure;
        if (tryBuildClosedTURealization(editIndex, closure)) {
          changed = true;
          break;
        }
        changed = false;
        editIndex = norm.size();
        break;
      }
      cursor = edit.end;
    }
    if (!changed)
      break;
  }

  // The byte-edit emission boundary is proof-gated. By the time an edit
  // reaches this function, every non-terminal artifact it composes must
  // already carry normalized accepted-result carriers that are fully
  // discharged for emitted source text. Do not emit any edit whose carriers
  // are missing or not yet discharged.
  if (!AuditAcceptedEditProofs(norm, "emit/nonterminal", emissionOwner,
                               ownerIncludeId, edits, originalFileText))
    return originalFileText.str();

  const size_t n = originalFileText.size();

  // Validate the normalized edit set before appending any bytes or recording
  // side effects. The applicator has only one sound composition law here:
  // edits must be in bounds and non-overlapping in original-file byte space.
  // If that law is violated, do not guess whether an insertion should be
  // merged into, ordered around, or shadowed by a replacement. Fail closed to
  // the explicit terminal fallback instead.
  // Record which A tokens an uncomposable edit came from whenever the edit
  // knows them.  Two edits colliding in source-byte space is a property of the
  // hunk partition, so the token range is what lets the caller tell a failure a
  // different alignment could still repair from one no anchor placement
  // reaches.  An edit without direct TU hunk provenance names nothing and
  // leaves the request unattributed exactly as before.
  auto uncomposableEditFailure = [](const TextEdit &edit, size_t editIndex) {
    if (!edit.directTUHunkAStart || !edit.directTUHunkAEnd)
      return MakeTerminalFallbackProofFailure(
          TerminalFallbackObligationKind::EmissionEditSetComposable,
          TerminalFallbackFailureReason::UncomposableEmissionEditSet);
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::EmissionEditSetComposable,
        TerminalFallbackFailureReason::UncomposableEmissionEditSet,
        TerminalFallbackFailureContext::ForHunkTokenEnvelope(
            edit.directTUHunkIndex.value_or(editIndex),
            *edit.directTUHunkAStart, *edit.directTUHunkAEnd,
            edit.directTUHunkBStart.value_or(0),
            edit.directTUHunkBEnd.value_or(0)));
  };

  uint64_t validatedCursor = 0;
  for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
    const TextEdit &e = norm[editIndex];
    if (e.end < e.start || e.end > n) {
      terminalSink_.RequestTerminalFallback(
          uncomposableEditFailure(e, editIndex), "edits/apply",
          llvm::formatv("bad normalized edit bounds in {0}: edit#{1}=[{2},{3}) "
                        "fileLen={4} textLen={5}",
                        emissionOwner, editIndex, e.start, e.end, n,
                        e.text.size())
              .str());
      return originalFileText.str();
    }
    if (e.start < validatedCursor) {
      terminalSink_.RequestTerminalFallback(
          uncomposableEditFailure(e, editIndex), "edits/apply",
          llvm::formatv("overlapping normalized edits in {0}: previousEnd={1} "
                        "edit#{2}=[{3},{4}) textLen={5}",
                        emissionOwner, validatedCursor, editIndex, e.start,
                        e.end, e.text.size())
              .str());
      return originalFileText.str();
    }
    validatedCursor = e.end;
  }

  // Assemble the final file by copying original slices between the normalized
  // edits and splicing each replacement at its staged byte range.
  SmallString<0> out;
  out.reserve(originalFileText.size() + 128);
  std::optional<PendingResync> pending = std::nullopt;

  uint64_t cursor = 0;

  for (const auto &e : norm) {
    // Copy the untouched source before this edit. If a previous replacement
    // could not safely emit its #line resync locally, this copy step is also
    // the next opportunity to flush that pending resync at a safe boundary.
    pending = AppendOriginalSliceWithPending(
        out, originalFileText, cursor, e.start, std::move(pending),
        sourceMappingOwner, ownerIncludeId, lineControlPruneCandidates,
        lineControlSourceMappings);

    // Record expanded macro roots that actually made it into the emitted text.
    // Counter stabilization uses this to force later __COUNTER__-sensitive
    // macro occurrences when needed.
    if (appliedExpandedMacroRootIds && e.expandedMacroRootId)
      appliedExpandedMacroRootIds->insert(*e.expandedMacroRootId);

    const uint64_t emittedBegin = static_cast<uint64_t>(out.size());
    out.append(e.text);
    if (lineControlPruneCandidates)
      appendShiftedLineControlPruneCandidates(*lineControlPruneCandidates,
                                              e.lineControlPruneCandidates,
                                              emittedBegin);
    if (lineControlSourceMappings)
      appendShiftedLineControlSourceMappings(*lineControlSourceMappings,
                                             e.lineControlSourceMappings,
                                             emittedBegin);

    // Record only the replacement bytes written by this edit. Copied original
    // slices and emitted #line/resync material are intentionally outside the
    // mapped source range because they are not materialized B edit payload.
    if (materializedEditMappings && e.materializesNoBPayload) {
      // A replacement proved to realize no B bytes contributes no edit-map row,
      // exactly like a copied original slice or an emitted #line resync. The
      // certificate is what distinguishes it from an edit that never reached a
      // B-side certifier at all; that case still fails closed below.
      REFOLD_LOG_TRACE("edit-map",
                       "B-payload-free edit in {0} at source=[{1},{2}) emits "
                       "{3} byte(s) of preprocessor state and is omitted from "
                       "the materialized edit map",
                       emissionOwner, e.start, e.end, e.text.size());
    } else if (materializedEditMappings) {
      std::optional<std::pair<uint64_t, uint64_t>> bRange =
          TextEditMaterializedBByteRange(e);
      if (!bRange) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::EmissionArtifactDischarged,
                TerminalFallbackFailureReason::UndischargedEmissionArtifact),
            "edit-map",
            llvm::formatv("emitted edit in {0} at source=[{1},{2}) lacks a "
                          "deterministic B-side materialization range",
                          emissionOwner, e.start, e.end)
                .str());
        materializedEditMappings->clear();
        return originalFileText.str();
      }
      std::optional<std::pair<uint64_t, uint64_t>> outRange =
          TextEditMaterializedOutputTextRange(e);
      if (!outRange) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::EmissionArtifactDischarged,
                TerminalFallbackFailureReason::UndischargedEmissionArtifact),
            "edit-map",
            llvm::formatv("emitted edit in {0} at source=[{1},{2}) carries "
                          "invalid refolded-output materialization range",
                          emissionOwner, e.start, e.end)
                .str());
        materializedEditMappings->clear();
        return originalFileText.str();
      }
      materializedEditMappings->push_back(MaterializedEditMapping{
          bRange->first, bRange->second, emittedBegin + outRange->first,
          emittedBegin + outRange->second});
    }

    // Carry any resync that the edit itself could not emit locally. It will be
    // discharged by the next copied original slice, or by the final tail copy.
    if (e.pending) {
      pending = e.pending;
    }

    cursor = e.end;
  }

  // Copy the final untouched tail and flush any pending line resync that
  // survived the last replacement.
  pending = AppendOriginalSliceWithPending(
      out, originalFileText, cursor, n, std::move(pending), sourceMappingOwner,
      ownerIncludeId, lineControlPruneCandidates, lineControlSourceMappings);

  auto shiftLineControlMetadataAfterInsertion = [&](uint64_t pos,
                                                    uint64_t len) {
    if (len == 0)
      return;

    if (lineControlPruneCandidates) {
      for (FinalLineControlPruneCandidate &candidate :
           *lineControlPruneCandidates) {
        if (candidate.finalBegin >= pos) {
          candidate.finalBegin += len;
          candidate.finalEnd += len;
        } else if (candidate.finalEnd > pos) {
          // A synthetic directive should never straddle an independently
          // inserted repair.  Preserve a conservative range if it does rather
          // than creating overlapping stale coordinates.
          candidate.finalEnd += len;
        }
      }
    }

    if (lineControlSourceMappings) {
      std::vector<FinalLineControlSourceMapping> adjusted;
      adjusted.reserve(lineControlSourceMappings->size() + 1);
      for (FinalLineControlSourceMapping mapping : *lineControlSourceMappings) {
        if (mapping.finalEnd <= pos) {
          adjusted.push_back(std::move(mapping));
          continue;
        }
        if (mapping.finalBegin >= pos) {
          mapping.finalBegin += len;
          mapping.finalEnd += len;
          adjusted.push_back(std::move(mapping));
          continue;
        }

        // The insertion splits a copied source slice.  Keep precise provenance
        // for the surviving prefix and suffix; the synthetic #line bytes in the
        // middle intentionally receive no source mapping.
        const uint64_t sourceSplit =
            mapping.sourceBegin + (pos - mapping.finalBegin);
        FinalLineControlSourceMapping prefix = mapping;
        prefix.finalEnd = pos;
        prefix.sourceEnd = sourceSplit;
        if (prefix.finalBegin < prefix.finalEnd)
          adjusted.push_back(std::move(prefix));

        FinalLineControlSourceMapping suffix = mapping;
        suffix.finalBegin = pos + len;
        suffix.finalEnd += len;
        suffix.sourceBegin = sourceSplit;
        if (suffix.finalBegin < suffix.finalEnd)
          adjusted.push_back(std::move(suffix));
      }
      *lineControlSourceMappings = std::move(adjusted);
      deduplicateLineControlSourceMappings(*lineControlSourceMappings);
    }
  };

  auto sourceOffsetToFinalOffset =
      [&](uint64_t sourceOffset) -> std::optional<uint64_t> {
    if (!lineControlSourceMappings)
      return std::nullopt;
    for (const FinalLineControlSourceMapping &mapping :
         *lineControlSourceMappings) {
      if (mapping.ownerIncludeId != ownerIncludeId)
        continue;
      if (mapping.physicalFile != sourceMappingOwner)
        continue;
      if (sourceOffset < mapping.sourceBegin ||
          sourceOffset > mapping.sourceEnd)
        continue;
      if (sourceOffset == mapping.sourceEnd)
        return mapping.finalEnd;
      return mapping.finalBegin + (sourceOffset - mapping.sourceBegin);
    }
    return std::nullopt;
  };

  struct ConditionalJoinLineRepair {
    uint64_t sourceOffset = 0;
    uint64_t finalOffset = 0;
    std::string directive;
  };

  std::vector<ConditionalJoinLineRepair> joinRepairs;
  if (lineDirs_.Enabled() && lineControlSourceMappings &&
      !sourceMappingOwner.empty()) {
    for (const RefoldModel::CondGroup *group :
         model_.GetCondGroups(emissionOwner, ownerIncludeId)) {
      if (!group || group->file != emissionOwner ||
          group->parentIncludeId != ownerIncludeId)
        continue;

      bool groupContainsEdit = false;
      for (const TextEdit &edit : norm) {
        const bool zeroWidthInside =
            edit.start == edit.end && group->ContainsByte(edit.start);
        const bool overlap =
            edit.start < group->groupE && edit.end > group->groupB;
        if (zeroWidthInside || overlap) {
          groupContainsEdit = true;
          break;
        }
      }
      if (!groupContainsEdit)
        continue;

      // Find the first line-state observer after the group that still exists in
      // the emitted text.
      //
      // A recorded observer whose source bytes an edit replaced is not an
      // observer of the output: those bytes carry the edited preprocessed
      // stream's already-expanded value, a constant that no emitted `#line` can
      // change.  Skipping it and continuing past that edit is therefore exact,
      // not a relaxation -- and it is required, because a `#line` cannot be
      // placed at an offset the output does not contain.
      //
      // The skip is authorized only when a normalized edit demonstrably covers
      // the observer.  An offset that is unmapped for any other reason means
      // the source-mapping inventory is incomplete, which stays fail-closed
      // below.
      std::optional<LineStateObserverSite> firstObserver;
      std::optional<uint64_t> finalOffset;
      for (uint64_t searchFrom = group->groupE;;) {
        firstObserver = lineControlProof_.FirstOwnerSuffixLineStateObserverSite(
            ownerIncludeId, emissionOwner, searchFrom);
        if (!firstObserver || !firstObserver->demand.Any())
          break;

        // If the observer is still inside the conditional group, an arm-local
        // repair is the only statement that can dominate it.  The join repair
        // is required only for observers reached after the group has rejoined.
        if (firstObserver->offset < group->groupE)
          break;

        finalOffset = sourceOffsetToFinalOffset(firstObserver->offset);
        if (finalOffset)
          break;

        const TextEdit *consumingEdit = nullptr;
        for (const TextEdit &edit : norm) {
          if (edit.start < edit.end && edit.start <= firstObserver->offset &&
              firstObserver->offset < edit.end) {
            consumingEdit = &edit;
            break;
          }
        }
        if (!consumingEdit || consumingEdit->end <= searchFrom)
          break;

        REFOLD_LOG_TRACE(
            "linedir/conditional-join",
            "{0} group#{1}: observer at {2} was replaced by the edit at "
            "[{3},{4}); resuming the search after it",
            emissionOwner, group->id, firstObserver->offset,
            consumingEdit->start, consumingEdit->end);
        searchFrom = consumingEdit->end;
        firstObserver.reset();
      }

      if (!firstObserver || !firstObserver->demand.Any())
        continue;
      if (firstObserver->offset < group->groupE)
        continue;
      const OwnerStateBoundary observerBoundary =
          OwnerStateBoundary::FromSource(
              OwnerSourceRange::From(emissionOwner, firstObserver->offset,
                                     firstObserver->offset, ownerIncludeId));

      auto forEachLineControlDemandComponent =
          [&](const LineStateObserverDemand &componentDemand, auto &&fn) {
            if (componentDemand.needsLine)
              fn(OwnerStateComponent::LineNumber);
            if (componentDemand.needsFile)
              fn(OwnerStateComponent::FileState);
            if (componentDemand.needsFileName)
              fn(OwnerStateComponent::FileName);
          };

      if (!finalOffset) {
        forEachLineControlDemandComponent(
            firstObserver->demand, [&](OwnerStateComponent component) {
              const std::string detail =
                  llvm::formatv("cannot map conditional join observer in {0}: "
                                "group#{1} observerSource={2}",
                                emissionOwner, group->id, firstObserver->offset)
                      .str();
              (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
                  observerBoundary, component, StateMutationKind::Replayed,
                  ownerStateProof_.BuildStateTransitionWitness(
                      SuffixStabilityWitnessKind::TerminalStateFailure,
                      component, observerBoundary, detail),
                  "linedir/conditional-join", detail,
                  /*requireKnownObserver=*/true);
            });
        return originalFileText.str();
      }

      LineDirectiveLocation loc =
          LineDirectiveInserter::LogicalLocationAtOffset(
              originalFileText, firstObserver->offset, emissionOwner, model_,
              emissionOwner, ownerIncludeId);

      // Conditional-join repairs must describe the logical line state that
      // reaches the first suffix observer after the conditional group rejoins.
      // The lexical scanner above can reconstruct ordinary source-local #line
      // state, but producer-backed recovery is needed for source-authored #line
      // operands whose evaluated state was recorded by the producer.

      if (!loc.producerProven) {
        if (std::optional<LineDirectiveLocation> producerLoc =
                lineControlProof_.ProducerBackedLineControlLocationAt(
                    originalFileText, emissionOwner, ownerIncludeId,
                    firstObserver->offset, firstObserver->offset)) {
          loc = std::move(*producerLoc);
        }
      }

      std::string directive =
          lineDirs_.FormatLineDirective(loc.lineNo, loc.fileSpelling);
      if (directive.empty())
        continue;

      auto finalLineBeginForOffset = [&](uint64_t offset) -> uint64_t {
        const uint64_t boundedOffset =
            std::min<uint64_t>(offset, static_cast<uint64_t>(out.size()));
        uint64_t lineBegin = boundedOffset;
        while (lineBegin > 0 && out[lineBegin - 1] != '\n')
          --lineBegin;
        return lineBegin;
      };

      auto immediatelyPrecededBySameLineDirective =
          [&](uint64_t insertionOffset, StringRef directiveText) -> bool {
        if (insertionOffset == 0 || directiveText.empty())
          return false;

        uint64_t prevLineEnd = insertionOffset;
        if (prevLineEnd > 0 && out[prevLineEnd - 1] == '\n')
          --prevLineEnd;
        uint64_t prevLineBegin = prevLineEnd;
        while (prevLineBegin > 0 && out[prevLineBegin - 1] != '\n')
          --prevLineBegin;

        StringRef prevLine(out.data() + prevLineBegin,
                           static_cast<size_t>(prevLineEnd - prevLineBegin));
        return prevLine.trim() == directiveText.trim();
      };

      const uint64_t insertionOffset = finalLineBeginForOffset(*finalOffset);
      StringRef currentOutAtInsertion(out.data(),
                                      static_cast<size_t>(insertionOffset));

      auto previousLineControlIsInsideThisConditionalGroup = [&]() -> bool {
        std::optional<std::pair<uint64_t, uint64_t>> previousDirective =
            findLastLineControlDirectiveRangeBefore(
                StringRef(out.data(), out.size()), insertionOffset);
        if (!previousDirective)
          return false;

        std::optional<uint64_t> groupFinalBegin =
            sourceOffsetToFinalOffset(group->groupB);
        std::optional<uint64_t> groupFinalEnd =
            sourceOffsetToFinalOffset(group->groupE);
        if (!groupFinalBegin || !groupFinalEnd)
          return false;

        return previousDirective->first >= *groupFinalBegin &&
               previousDirective->second <= *groupFinalEnd;
      };

      if (!LineDirectiveInserter::ShouldEmitLineDirective(
              currentOutAtInsertion, loc.fileSpelling, loc.lineNo, directive)) {
        if (!previousLineControlIsInsideThisConditionalGroup()) {
          forEachLineControlDemandComponent(
              firstObserver->demand, [&](OwnerStateComponent component) {
                const std::string detail =
                    llvm::formatv("suppress post-conditional #line in {0}: "
                                  "group#{1} observerSource={2}; current "
                                  "stream state is equivalent",
                                  emissionOwner, group->id,
                                  firstObserver->offset)
                        .str();
                (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
                    observerBoundary, component,
                    StateMutationKind::PreservedAcrossReplacement,
                    ownerStateProof_.BuildStateTransitionWitness(
                        SuffixStabilityWitnessKind::StateRepair, component,
                        observerBoundary, detail),
                    "linedir/conditional-join", detail,
                    /*requireKnownObserver=*/true);
              });
          continue;
        }
      }

      if (immediatelyPrecededBySameLineDirective(insertionOffset, directive)) {
        forEachLineControlDemandComponent(
            firstObserver->demand, [&](OwnerStateComponent component) {
              const std::string detail =
                  llvm::formatv("suppress duplicate post-conditional #line in "
                                "{0}: group#{1} observerSource={2}",
                                emissionOwner, group->id, firstObserver->offset)
                      .str();
              (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
                  observerBoundary, component,
                  StateMutationKind::PreservedAcrossReplacement,
                  ownerStateProof_.BuildStateTransitionWitness(
                      SuffixStabilityWitnessKind::StateRepair, component,
                      observerBoundary, detail),
                  "linedir/conditional-join", detail,
                  /*requireKnownObserver=*/true);
            });
        continue;
      }

      forEachLineControlDemandComponent(
          firstObserver->demand, [&](OwnerStateComponent component) {
            const std::string detail =
                llvm::formatv("post-conditional synthetic #line repair in {0}: "
                              "group#{1} observerSource={2} final={3}",
                              emissionOwner, group->id, firstObserver->offset,
                              insertionOffset)
                    .str();
            (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
                observerBoundary, component, StateMutationKind::Replayed,
                ownerStateProof_.BuildStateTransitionWitness(
                    SuffixStabilityWitnessKind::StateRepair, component,
                    observerBoundary, detail),
                "linedir/conditional-join", detail,
                /*requireKnownObserver=*/true);
          });

      joinRepairs.push_back(ConditionalJoinLineRepair{
          firstObserver->offset, insertionOffset, std::move(directive)});
    }
  }

  if (!joinRepairs.empty()) {
    llvm::sort(joinRepairs, [](const ConditionalJoinLineRepair &lhs,
                               const ConditionalJoinLineRepair &rhs) {
      if (lhs.finalOffset != rhs.finalOffset)
        return lhs.finalOffset > rhs.finalOffset;
      if (lhs.sourceOffset != rhs.sourceOffset)
        return lhs.sourceOffset > rhs.sourceOffset;
      return lhs.directive < rhs.directive;
    });
    joinRepairs.erase(std::unique(joinRepairs.begin(), joinRepairs.end(),
                                  [](const ConditionalJoinLineRepair &lhs,
                                     const ConditionalJoinLineRepair &rhs) {
                                    return lhs.finalOffset == rhs.finalOffset &&
                                           lhs.directive == rhs.directive;
                                  }),
                      joinRepairs.end());

    for (const ConditionalJoinLineRepair &repair : joinRepairs) {
      out.insert(out.begin() + static_cast<size_t>(repair.finalOffset),
                 repair.directive.begin(), repair.directive.end());
      shiftLineControlMetadataAfterInsertion(
          repair.finalOffset, static_cast<uint64_t>(repair.directive.size()));
    }
  }

  if (lineControlPruneCandidates)
    deduplicateLineControlPruneCandidates(*lineControlPruneCandidates);
  if (lineControlSourceMappings)
    deduplicateLineControlSourceMappings(*lineControlSourceMappings);
  return std::string(out.str());
}

std::optional<RefoldTextEditAssembler::PendingResync>
RefoldTextEditAssembler::AppendOriginalSliceWithPending(
    SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
    uint64_t to, std::optional<RefoldTextEditAssembler::PendingResync> pending,
    StringRef emissionOwner, std::optional<uint64_t> ownerIncludeId,
    std::vector<FinalLineControlPruneCandidate> *lineControlPruneCandidates,
    std::vector<FinalLineControlSourceMapping> *lineControlSourceMappings)
    const {
  auto appendMappedOriginalSlice = [&](uint64_t begin, uint64_t end) {
    if (begin >= end)
      return;
    const uint64_t finalBegin = static_cast<uint64_t>(out.size());
    auto slice =
        original.slice(static_cast<size_t>(begin), static_cast<size_t>(end));
    out.append(slice.begin(), slice.end());
    const uint64_t finalEnd = static_cast<uint64_t>(out.size());
    if (lineControlSourceMappings && !emissionOwner.empty()) {
      lineControlSourceMappings->push_back(FinalLineControlSourceMapping{
          finalBegin, finalEnd, emissionOwner.str(), begin, end,
          ownerIncludeId});
    }
  };

  // Fast path: if there is no pending #line correction, or line directives are
  // disabled, this helper is just a straight copy of the untouched source
  // slice.
  if (!pending || !lineDirs_.Enabled()) {
    appendMappedOriginalSlice(from, to);
    return std::nullopt;
  }

  if (pending->deferToConditionalJoin) {
    appendMappedOriginalSlice(from, to);
    return pending;
  }

  auto checkPendingLineControlRepair =
      [&](uint64_t sourceOffset, StateMutationKind mutation, StringRef detail) {
        const OwnerStateBoundary boundary = OwnerStateBoundary::FromSource(
            OwnerSourceRange::From(pending->fileSpellingForDir, sourceOffset,
                                   sourceOffset, pending->ownerIncludeId));
        return ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
            boundary, OwnerStateComponent::LineNumber, mutation,
            ownerStateProof_.BuildStateTransitionWitness(
                SuffixStabilityWitnessKind::StateRepair,
                OwnerStateComponent::LineNumber, boundary, detail),
            "line/pending", detail, /*requireKnownObserver=*/false);
      };

  uint64_t i = from;

  auto canFlushBeforeUntouchedDirectiveLine = [&](size_t pos) -> bool {
    if (pos >= original.size())
      return false;

    // Find the physical start of the current line and require that `pos` is
    // still inside that line's indentation prefix.
    size_t lineStart = pos;
    while (lineStart > 0 && original[lineStart - 1] != '\n')
      --lineStart;

    if (!stringutils::isIndentOnly(original, lineStart, pos))
      return false;

    size_t lineEnd = original.find('\n', pos);
    if (lineEnd == StringRef::npos)
      lineEnd = original.size();

    // A preprocessor directive may be preceded by horizontal whitespace. If the
    // first non-whitespace character is '#', flushing before the indentation is
    // still a directive-line-safe resync point.
    size_t firstNonWs = pos;
    while (firstNonWs < lineEnd && stringutils::isWs(original[firstNonWs]) &&
           original[firstNonWs] != '\n')
      ++firstNonWs;

    return firstNonWs < lineEnd && original[firstNonWs] == '#';
  };

  auto logicalLocationForPendingFlush =
      [&](uint64_t pos) -> std::optional<LineDirectiveLocation> {
    LineDirectiveLocation loc = LineDirectiveInserter::LogicalLocationAtOffset(
        original, static_cast<size_t>(pos), pending->fileSpellingForDir, model_,
        pending->fileSpellingForDir, pending->ownerIncludeId);
    if (loc.producerProven)
      return loc;

    // If the local scanner found an unmodeled source #line in the bytes copied
    // after the edit, the pending state can no longer be propagated by counting
    // newlines: that directive may have overwritten the logical stream state.
    if (loc.unprovenLineControlDirectiveOffset &&
        *loc.unprovenLineControlDirectiveOffset >= pending->resumeOffset &&
        *loc.unprovenLineControlDirectiveOffset < pos)
      return std::nullopt;

    // The pending object was created only after ApplyResyncOrPend proved the
    // logical state at resumeOffset.  If the eventual flush point is later in
    // untouched source, and no intervening unmodeled line-control directive was
    // found, advance that proved state by the physical non-spliced newlines
    // copied from the original between resumeOffset and pos.  This handles
    // source-authored #line directives whose operands used imported macro
    // state: the local scanner cannot re-evaluate them, but the producer
    // already did.
    if (pending->resumeLineNo == 0 || pos < pending->resumeOffset)
      return std::nullopt;

    const size_t delta = stringutils::countNonSplicedNewlines(
        original, pending->resumeOffset, pos);
    return LineDirectiveLocation(pending->resumeFileSpelling,
                                 pending->resumeLineNo + delta, true);
  };

  // Prefer to flush the pending resync before copying this slice when the
  // emitted output is already at BOL and the untouched input also resumes at a
  // safe line boundary. This handles both true BOL and the indentation prefix
  // of an untouched preprocessor directive line.
  if (stringutils::outAtBOL(StringRef(out.data(), out.size())) &&
      (stringutils::isBOL(original, static_cast<size_t>(from)) ||
       canFlushBeforeUntouchedDirectiveLine(static_cast<size_t>(from)))) {
    std::optional<LineDirectiveLocation> loc =
        logicalLocationForPendingFlush(from);
    if (!loc) {
      appendMappedOriginalSlice(from, to);
      return std::nullopt;
    }

    std::string directive =
        lineDirs_.FormatLineDirective(loc->lineNo, loc->fileSpelling);

    StringRef currentOut(out.data(), out.size());

    // Avoid emitting a redundant directive when the current output state
    // already represents the requested file/line location.
    if (LineDirectiveInserter::ShouldEmitLineDirective(
            currentOut, loc->fileSpelling, loc->lineNo, directive)) {
      (void)checkPendingLineControlRepair(
          from, StateMutationKind::MovedLater,
          llvm::formatv("flush pending synthetic #line at slice begin {0}",
                        from)
              .str());
      const uint64_t begin = static_cast<uint64_t>(out.size());
      out.append(directive.begin(), directive.end());
      const uint64_t end = static_cast<uint64_t>(out.size());
      if (lineControlPruneCandidates &&
          pending->finalLineControlPruneEligible) {
        lineControlPruneCandidates->push_back(
            makeSyntheticLineControlPruneCandidate(
                begin, end, FinalLineDirective::Origin::SyntheticNewlineResync,
                FinalLineControlOwnerKey(pending->fileSpellingForDir,
                                         pending->ownerIncludeId),
                FinalLineControlObligation::CosmeticSyntheticResync));
      }
    } else {
      (void)checkPendingLineControlRepair(
          from, StateMutationKind::PreservedAcrossReplacement,
          llvm::formatv("suppress pending #line at slice begin {0}; "
                        "current output state is equivalent",
                        from)
              .str());
    }

    appendMappedOriginalSlice(from, to);
    return std::nullopt;
  }

  // If the slice does not begin at a safe flush point, copy forward until the
  // first real newline boundary. Line-spliced newlines are not safe because
  // they do not end the logical source line.
  while (i < to) {
    size_t nl = original.find('\n', i);
    if (nl == llvm::StringRef::npos || nl >= static_cast<size_t>(to))
      break;

    appendMappedOriginalSlice(i, static_cast<uint64_t>(nl + 1));

    i = nl + 1;

    if (!stringutils::isLineSplice(original, nl)) {
      std::optional<LineDirectiveLocation> loc =
          logicalLocationForPendingFlush(i);
      if (!loc) {
        pending = std::nullopt;
        break;
      }

      std::string directive =
          lineDirs_.FormatLineDirective(loc->lineNo, loc->fileSpelling);

      StringRef currentOut(out.data(), out.size());
      if (LineDirectiveInserter::ShouldEmitLineDirective(
              currentOut, loc->fileSpelling, loc->lineNo, directive)) {
        (void)checkPendingLineControlRepair(
            i, StateMutationKind::MovedLater,
            llvm::formatv("flush pending synthetic #line at safe newline {0}",
                          i)
                .str());
        const uint64_t begin = static_cast<uint64_t>(out.size());
        out.append(directive.begin(), directive.end());
        const uint64_t end = static_cast<uint64_t>(out.size());
        if (lineControlPruneCandidates &&
            pending->finalLineControlPruneEligible) {
          lineControlPruneCandidates->push_back(
              makeSyntheticLineControlPruneCandidate(
                  begin, end,
                  FinalLineDirective::Origin::SyntheticNewlineResync,
                  FinalLineControlOwnerKey(pending->fileSpellingForDir,
                                           pending->ownerIncludeId),
                  FinalLineControlObligation::CosmeticSyntheticResync));
        }
      } else {
        (void)checkPendingLineControlRepair(
            i, StateMutationKind::PreservedAcrossReplacement,
            llvm::formatv("suppress pending #line at safe newline {0}; "
                          "current output state is equivalent",
                          i)
                .str());
      }

      pending = std::nullopt;
      break;
    }
  }

  // Copy any bytes after the flush point, or the whole slice tail if no safe
  // flush point was found. In the latter case, return the still-pending resync
  // so a later original slice can discharge it.
  if (i < to) {
    appendMappedOriginalSlice(i, to);
  }
  return pending;
}

std::optional<TextEdit>
RefoldTextEditAssembler::BuildDirectTUHunkTextEdit(
    const diffutils::Hunk &h, uint64_t hunkIndex,
    const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
    StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
    std::optional<uint64_t> materializedBByteBegin,
    std::optional<uint64_t> materializedBByteEnd,
    AcceptedPathKind acceptedPath,
    std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment) const {
  std::optional<DirectTUHunkEditPlan> plan =
      tuEdits_.BuildDirectTUHunkEditPlan(
          h, hunkIndex, span, std::move(resync), acceptedPayload, rawTUStart,
          rawTUEnd, materializedBByteBegin, materializedBByteEnd, acceptedPath,
          std::move(insertionAnchorAdjustment));
  if (!plan)
    return std::nullopt;

  assert(plan->resync && "direct TU hunk edit plan requires resync payload");
  auto spanBytes = plan->span.byteRange();
  TextEdit edit{spanBytes.first,
                spanBytes.second,
                std::move(plan->resync->text),
                std::move(plan->resync->pending),
                std::nullopt,
                {},
                {},
                {}};
  edit.lineControlPruneCandidates =
      std::move(plan->resync->lineControlPruneCandidates);
  edit.isDirectTUHunkEdit = true;
  edit.directTUHunkIndex = plan->hunkIndex;
  edit.directTUHunkAStart = plan->hunk.aStart;
  edit.directTUHunkAEnd = plan->hunk.aEnd;
  edit.directTUHunkBStart = plan->hunk.bStart;
  edit.directTUHunkBEnd = plan->hunk.bEnd;
  edit.directTURawStart = plan->rawTUStart;
  edit.directTURawEnd = plan->rawTUEnd;
  edit.directTUFinalStart = spanBytes.first;
  edit.directTUFinalEnd = spanBytes.second;

  // An ordinary direct-TU edit carries no protected-source authority. Exact
  // macro transitions inside the provisional carrier are rediscovered and
  // discharged only by RefoldMacroStateRepairPlanner; this assembly boundary
  // deliberately has no conversion from planning evidence to capability.

  if (plan->materializedBByteBegin && plan->materializedBByteEnd)
    textEditCertifier_.CertifyTextEditMaterializedBByteRange(
        edit, *plan->materializedBByteBegin, *plan->materializedBByteEnd);
  AcceptedResultCandidate candidate =
      acceptedCandidateBuilder_.BuildAcceptedTUTextEditCandidate(
          plan->acceptedPath, plan->hunk, plan->span,
          /*structuralBinding=*/nullptr, plan->acceptedPayload);
  ownerRealizationProofBuilder_.AttachMixedOwnerTilingWitnessForTokenEnvelope(
      candidate.proofSummary, plan->hunk.aStart, plan->hunk.aEnd,
      plan->hunk.bStart, plan->hunk.bEnd);
  ownerRealizationProofBuilder_.AttachStandardWitnesses(candidate);
  textEditCertifier_.AttachAcceptedResultCarrier(edit, candidate);
  return edit;
}

} // namespace refold
} // namespace clang

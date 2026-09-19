//===--- RefoldDirectTUEditBuilder.cpp --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Construction of the direct translation-unit byte-span edit that realizes one
// token hunk over a TU byte span another proof has already accepted.
//
// RefoldTUAnchorProof proves where the source may be edited; this builder
// constructs the exact replacement that realizes B there; the text-edit
// assembler certifies it; RefoldEngine chooses whether to take this path.
//
//===----------------------------------------------------------------------===//

#include "edit/RefoldDirectTUEditBuilder.h"

#include "edit/RefoldTUAnchorProof.h"
#include "edit/RefoldTUEditPlanner.h"
#include "edit/RefoldTextEditAssembler.h"
#include "line-control/RefoldLineControlProof.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "model/RefoldModel.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "sideband/RefoldSidebandPragmaEdits.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "source/TokenTextHelpers.h"
#include "support/RefoldLog.h"
#include "support/StringUtils.h"

#include <cassert>
#include <string>

using namespace llvm;

namespace clang {
namespace refold {

RefoldDirectTUEditBuilder::RefoldDirectTUEditBuilder(Dependencies deps)
    : deps_(deps) {}

std::optional<TextEdit> RefoldDirectTUEditBuilder::Build(
    const diffutils::Hunk &h, size_t hunkIndex, bool isDel, StringRef tuPath,
    StringRef tuBytes, std::pair<uint64_t, uint64_t> span,
    AcceptedPathKind acceptedPath) const {
  // The mapped and conservative realizations differ in exactly three
  // respects, all derived from the accepted path: whether a whitespace-only
  // span keeps its own bytes, whether a materialized-include deferral needs
  // visible replay text, and the path the certified edit records.
  assert((acceptedPath == AcceptedPathKind::TUByteSpanMappedEdit ||
          acceptedPath == AcceptedPathKind::TUByteSpanConservativeEdit) &&
         "direct TU byte-span edit with a non-byte-span accepted path");
  const bool mapped = acceptedPath == AcceptedPathKind::TUByteSpanMappedEdit;

  std::string repl;
  const uint64_t rawTUStart = span.first;
  const uint64_t rawTUEnd = span.second;
  std::optional<uint64_t> materializedBByteBegin;
  std::optional<uint64_t> materializedBByteEnd;
  if (auto bBytes =
          deps_.sourceMapper.BTokenRangeToByteRange(h.bStart, h.bEnd)) {
    materializedBByteBegin = bBytes->first;
    materializedBByteEnd = bBytes->second;
  }
  const PrintedPragmaInsertionPlacement pragmaPlacement =
      isDel ? PrintedPragmaInsertionPlacement()
            : PlaceTUInsertionAmongPrintedPragmas(h, span.first);
  if (pragmaPlacement.kind == PrintedPragmaInsertionPlacement::Kind::Refused)
    return std::nullopt;
  if (pragmaPlacement.kind == PrintedPragmaInsertionPlacement::Kind::Placed) {
    span = {pragmaPlacement.tuByteOffset, pragmaPlacement.tuByteOffset};
    if (pragmaPlacement.bByteEnd)
      materializedBByteEnd = *pragmaPlacement.bByteEnd;
  }
  if (isDel) {
    repl = "";
  } else {
    StringRef bSlice =
        h.isInsertOnly()
            ? InsertionEnvelope(h, pragmaPlacement)
            : refoldSliceExactTokenCoverage(deps_.bTokOff, deps_.bToks,
                                            deps_.bSource, h.bStart, h.bEnd);
    repl.assign(bSlice.data(), bSlice.data() + bSlice.size());
    if (h.isInsertOnly()) {
      // For pure insertions the source slice is the *token envelope*, not
      // just the exact token byte cover.  Zero-normal-token sideband lines
      // can live between the inserted ordinary tokens and the next normal
      // token.  Use the actual envelope bytes when partitioning replay so a
      // separately materialized sideband replacement/deletion is not also
      // emitted by this ordinary TU insertion.
      std::optional<uint64_t> envelopeBegin;
      std::optional<uint64_t> envelopeEnd;
      if (!bSlice.empty()) {
        envelopeBegin =
            static_cast<uint64_t>(bSlice.data() - deps_.bSource.data());
        envelopeEnd = *envelopeBegin + static_cast<uint64_t>(bSlice.size());
      }
      repl = stripSeparatelyOwnedSidebandReplay(deps_.sidebandPragmaEdits, repl,
                                                envelopeBegin, envelopeEnd);
    }
  }

  // This patch inserts B text at a zero-width TU site: the TU span is
  // empty, but the hunk contributes one or more B tokens. Token-envelope
  // byte ranges begin at the first inserted token, so they do not include
  // any spaces or tabs that appear immediately before that token in B on
  // the same line. Preserve those preceding spaces/tabs when forming the
  // inserted text, unless equivalent spacing is already present immediately
  // to the left of the insertion point in the TU.
  if (!isDel && span.first == span.second && h.bStart < h.bEnd &&
      h.bStart > 0) {
    const size_t bTokStart = static_cast<size_t>(h.bStart);
    const size_t b0 = deps_.bTokOff[bTokStart];
    size_t p = b0;
    while (p > 0) {
      char c = deps_.bSource[p - 1];
      if (c == ' ' || c == '\t') {
        --p;
        continue;
      }
      break;
    }
    if (p < b0) {
      const bool tuHasSpaceLeft =
          span.first > 0 &&
          (tuBytes[span.first - 1] == ' ' || tuBytes[span.first - 1] == '\t');
      if (!tuHasSpaceLeft) {
        repl.insert(0, std::string(deps_.bSource.data() + p, b0 - p));
        materializedBByteBegin = static_cast<uint64_t>(p);
      }
    }
  }

  bool consumedSeparatorGapForPunctuation = false;

  if (!isDel && h.isInsertOnly()) {
    // B attaches the inserted content to its left neighbour iff there is no
    // whitespace before the first inserted B token.
    const size_t bStartIdx = static_cast<size_t>(h.bStart);
    const bool replayAttachesLeftInB =
        bStartIdx < deps_.bTokOff.size() && deps_.bTokOff[bStartIdx] > 0 &&
        !stringutils::isWs(deps_.bSource[deps_.bTokOff[bStartIdx] - 1]);
    consumedSeparatorGapForPunctuation = maybeConsumeLeftSourceGapWhenBAttaches(
        deps_.model, deps_.pathIdentity, tuPath, tuBytes, span, StringRef(repl),
        replayAttachesLeftInB, deps_.lexLang);
  }

  deps_.tuEditPlanner.MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      h, tuPath, tuBytes, repl, span);

  const bool advancedOverSourceLineControlPrefix =
      pragmaPlacement.kind != PrintedPragmaInsertionPlacement::Kind::Placed &&
      maybeAdvanceTUInsertionPastSourceLineControlPrefix(
          deps_.tuAnchorProof, deps_.lineControlProof, h, tuPath, tuBytes,
          span);
  std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment =
      InsertionAnchorAdjustment(pragmaPlacement,
                                advancedOverSourceLineControlPrefix, rawTUStart,
                                span.first);

  // If we are replacing whitespace-only text in the TU, we prefer to
  // preserve the existing TU gap whitespace rather than introducing new
  // whitespace from B.
  bool replacingGap = false;
  if (span.first < span.second) {
    std::string original(tuBytes.data() + span.first,
                         tuBytes.data() + span.second);
    replacingGap = !original.empty() && stringutils::isWs(original);
    if (!mapped && replacingGap && !consumedSeparatorGapForPunctuation) {
      // Preserve exactly the gap as the replacement.
      repl = std::move(original);
    }
  } else if (span.second < span.first) {
    REFOLD_LOG_FATAL("tu/span", "invalid TU byte span: [{0},{1})", span.first,
                     span.second);
  }

  // If this patch replaces a non-empty whitespace gap in the TU, and the
  // replacement text does not already begin with whitespace, prefix a
  // single space so adjacent tokens remain separated. Add only one space,
  // even if the original gap was wider, to avoid duplicating spacing.
  if (replacingGap && !consumedSeparatorGapForPunctuation && !repl.empty() &&
      !stringutils::isWs(repl.front()))
    repl.insert(repl.begin(), ' ');

  // On the left, let refoldPadAtBoundaries add a space only when no replaced
  // TU gap already supplied whitespace; on the right, always allow padding if
  // the replacement would otherwise glue to the following TU text.
  std::string padded = refoldPadAtBoundaries(
      tuBytes, static_cast<size_t>(span.first),
      static_cast<size_t>(span.second), std::move(repl),
      /*allowLeft*/ !replacingGap, /*allowRight*/ true, deps_.lexLang);

  const bool skipLocalResync =
      tuInsertionBeforeMaterializedInclude(
          deps_.tuAnchorProof, deps_.model, deps_.sidebandPragmaEdits, h,
          tuPath, span,
          /*requireVisibleReplayText=*/mapped) ||
      deps_.lineControlProof.TUInsertionCanDeferResyncToConditionalJoin(
          advancedOverSourceLineControlPrefix, tuPath, span.second);

  ResyncOutcome ro =
      skipLocalResync ? ResyncOutcome(padded, std::nullopt)
                      : deps_.lineObserverLayout.ApplyResyncOrPend(
                            tuBytes, span.first, span.second, padded, tuPath);
  return deps_.textEditAssembler.BuildDirectTUHunkTextEdit(
      h, hunkIndex, span, std::move(ro), StringRef(padded), rawTUStart,
      rawTUEnd, materializedBByteBegin, materializedBByteEnd, acceptedPath,
      std::move(insertionAnchorAdjustment));
}

PrintedPragmaInsertionPlacement
RefoldDirectTUEditBuilder::PlaceTUInsertionAmongPrintedPragmas(
    const diffutils::Hunk &h, uint64_t baseAnchor) const {
  PrintedPragmaInsertionPlacement placement =
      placeTUInsertionAmongPrintedPragmas(
          deps_.tuAnchorProof, deps_.printedPragmaCarriers,
          deps_.sidebandPragmaLinePairings, h, deps_.bTokOff,
          deps_.tuSourceBytes, baseAnchor);
  switch (placement.kind) {
  case PrintedPragmaInsertionPlacement::Kind::NotApplicable:
    break;
  case PrintedPragmaInsertionPlacement::Kind::Placed:
    REFOLD_LOG_TRACE("tu/insertion",
                     "insertion A={0} B=[{1},{2}) placed at source byte {3} "
                     "(base anchor {4}) on the side of each preserved pragma "
                     "B prints it on; replaying B up to byte {5}",
                     h.aStart, h.bStart, h.bEnd, placement.tuByteOffset,
                     baseAnchor,
                     placement.bByteEnd ? std::to_string(*placement.bByteEnd)
                                        : std::string("<envelope end>"));
    break;
  case PrintedPragmaInsertionPlacement::Kind::Refused:
    REFOLD_LOG_TRACE("tu/insertion",
                     "insertion A={0} B=[{1},{2}) has a preserved pragma B "
                     "prints after it, but no exact source site between the "
                     "directives was proved; refusing the direct TU edit",
                     h.aStart, h.bStart, h.bEnd);
    break;
  }
  return placement;
}

StringRef RefoldDirectTUEditBuilder::InsertionEnvelope(
    const diffutils::Hunk &h,
    const PrintedPragmaInsertionPlacement &placement) const {
  if (placement.kind != PrintedPragmaInsertionPlacement::Kind::Placed ||
      !placement.bByteEnd)
    return refoldSliceTokenEnvelope(deps_.bTokOff, deps_.bSource, h.bStart,
                                    h.bEnd);
  const uint64_t begin = deps_.bTokOff[static_cast<size_t>(h.bStart)];
  return deps_.bSource.slice(begin, *placement.bByteEnd);
}

std::optional<TUInsertionAnchorAdjustment>
RefoldDirectTUEditBuilder::InsertionAnchorAdjustment(
    const PrintedPragmaInsertionPlacement &placement,
    bool advancedOverSourceLineControlPrefix, uint64_t rawTUStart,
    uint64_t anchor) {
  if (placement.kind == PrintedPragmaInsertionPlacement::Kind::Placed &&
      anchor != rawTUStart)
    return TUInsertionAnchorAdjustment{
        TUInsertionAnchorAdjustmentKind::PrintedPragmaPlacement, rawTUStart,
        anchor};
  if (advancedOverSourceLineControlPrefix)
    return TUInsertionAnchorAdjustment{
        TUInsertionAnchorAdjustmentKind::SourceLineControlPrefix, rawTUStart,
        anchor};
  return std::nullopt;
}

} // namespace refold
} // namespace clang

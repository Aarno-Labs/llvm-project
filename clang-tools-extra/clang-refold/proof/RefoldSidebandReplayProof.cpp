//===--- RefoldSidebandReplayProof.cpp ------------------------*- C++ -*-===//
//
// Sideband pragma replay proof and validation helpers.
//
// This file deliberately contains no RefoldEngine mutation or TextEdit staging:
// callers provide already-classified sideband proof records and receive either
// a structural validation failure, a replay-stripped payload, or a B-byte
// witness envelope.  The sideband validation reporter records classified
// terminal fallback requests through RefoldTerminalProofSink so TU and include
// paths share the same fail-closed policy.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldSidebandReplayProof.h"
#include "core/RefoldLog.h"
#include "proof/RefoldTerminalProofSink.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

namespace clang {
namespace refold {

std::optional<StringRef>
validateSidebandPragmaEditProof(const SidebandPragmaEdit &edit,
                                uint64_t bSize) {
  if (!edit.SourceHasClosureProof())
    return StringRef("missing one or more owner-local proof obligations");

  if (!edit.SourceIsValid())
    return StringRef("invalid source byte range");

  if (!edit.ReplayIsValid(bSize))
    return StringRef("invalid sideband B-byte envelope");

  return std::nullopt;
}

bool validateAndReportSidebandPragmaEditProof(
    const SidebandPragmaEdit &edit, uint64_t bSize,
    const RefoldTerminalProofSink &terminalSink, StringRef stage,
    bool traceSuccess) {
  const auto sourceRange = edit.SourceByteRange();
  const auto replayRange = edit.MaterializedBByteRange();

  // Keep the structural sideband predicate pure, but centralize the
  // fail-closed proof-to-terminal-fallback translation here.  Both TU and
  // include materialization paths reject the same invalid proof with the same
  // theorem-facing obligation and diagnostic envelope.
  if (std::optional<StringRef> failure =
          validateSidebandPragmaEditProof(edit, bSize)) {
    terminalSink.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::PragmaBoundaryKnown,
            TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary),
        stage,
        llvm::formatv(
            "sideband owner-local proof invalid path='{0}' site=[{1},{2}) "
            "b=[{3},{4}): {5}",
            edit.SourcePath(), sourceRange.first, sourceRange.second,
            replayRange.first, replayRange.second, *failure)
            .str());
    return false;
  }

  if (traceSuccess) {
    REFOLD_LOG_TRACE(
        "proof/owner-local",
        "sideband proof ok stage={0} path='{1}' source=[{2},{3}) b=[{4},{5}) "
        "owner={6}",
        stage, edit.SourcePath(), sourceRange.first, sourceRange.second,
        replayRange.first, replayRange.second,
        edit.OwnerIncludeId()
            ? llvm::formatv("inc#{0}", *edit.OwnerIncludeId()).str()
            : std::string("TU"));
  }

  return true;
}

std::string stripSeparatelyOwnedSidebandReplay(
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits, StringRef replayText,
    std::optional<uint64_t> replayBByteBegin,
    std::optional<uint64_t> replayBByteEnd) {
  if (!replayBByteBegin || !replayBByteEnd ||
      *replayBByteEnd <= *replayBByteBegin || replayText.empty())
    return replayText.str();

  struct RemovalRange {
    uint64_t begin;
    uint64_t end;
  };

  llvm::SmallVector<RemovalRange, 4> removals;
  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits) {
    // A zero-width source-side pragma insertion is the one sideband class whose
    // visible B bytes may legitimately be carried by a surrounding ordinary
    // insertion island.  Non-insertion sideband edits have their own source
    // atom to replace/delete, so replaying the same raw B bytes through the
    // ordinary token envelope would duplicate that sideband line.
    if (sideband.SourceIsZeroWidthInsertion() ||
        !sideband.EmitsVisibleReplayText())
      continue;

    const auto sidebandB = sideband.MaterializedBByteRange();
    if (sidebandB.first < *replayBByteBegin ||
        *replayBByteEnd < sidebandB.second ||
        sidebandB.first >= sidebandB.second) {
      continue;
    }

    const uint64_t relBegin = sidebandB.first - *replayBByteBegin;
    const uint64_t relEnd = sidebandB.second - *replayBByteBegin;
    if (relEnd > replayText.size())
      continue;

    StringRef replaySlice = replayText.slice(static_cast<size_t>(relBegin),
                                             static_cast<size_t>(relEnd));
    if (replaySlice != sideband.ReplacementText())
      continue;

    removals.push_back({relBegin, relEnd});
  }

  if (removals.empty())
    return replayText.str();

  llvm::sort(removals, [](const RemovalRange &lhs, const RemovalRange &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    return lhs.end < rhs.end;
  });

  llvm::SmallVector<RemovalRange, 4> merged;
  for (const RemovalRange &range : removals) {
    if (range.begin >= range.end)
      continue;
    if (!merged.empty() && range.begin < merged.back().end)
      REFOLD_LOG_FATAL("pragma/sideband",
                       "overlapping separately-owned sideband replay ranges "
                       "[{0},{1}) and [{2},{3})",
                       merged.back().begin, merged.back().end, range.begin,
                       range.end);
    if (!merged.empty() && range.begin == merged.back().end) {
      merged.back().end = range.end;
      continue;
    }
    merged.push_back(range);
  }

  std::string out;
  out.reserve(replayText.size());
  uint64_t cursor = 0;
  for (const RemovalRange &range : merged) {
    out.append(replayText
                   .slice(static_cast<size_t>(cursor),
                          static_cast<size_t>(range.begin))
                   .str());
    cursor = range.end;
  }
  out.append(replayText.drop_front(static_cast<size_t>(cursor)).str());

  return out;
}

std::optional<std::pair<uint64_t, uint64_t>>
sidebandPragmaMaterializedBByteRangeForInclude(
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits, uint64_t includeId,
    uint64_t bSize) {
  std::optional<uint64_t> begin;
  std::optional<uint64_t> end;

  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits) {
    if (!sideband.TargetsInclude(includeId))
      continue;

    // Sideband-only header materialization can have an empty normal-token
    // cover.  In that case the ordinary include-cover resolver has no B-token
    // envelope to report to the edit-map writer, but each sideband edit already
    // carries the exact raw-B byte range that produced its replacement (or the
    // deterministic zero-length B anchor for a deletion).  Union those ranges
    // to make the final include replacement a fully witnessed emitted edit.
    if (!sideband.ReplayIsValid(bSize))
      return std::nullopt;

    const std::pair<uint64_t, uint64_t> bRange =
        sideband.MaterializedBByteRange();
    begin = begin ? std::min(*begin, bRange.first) : bRange.first;
    end = end ? std::max(*end, bRange.second) : bRange.second;
  }

  if (!begin || !end)
    return std::nullopt;
  return std::make_pair(*begin, *end);
}

bool sidebandInsertionReplayIsCoveredByPatchEnvelope(
    const SidebandPragmaEdit &sideband, uint64_t patchBByteBegin,
    uint64_t patchBByteEnd) {
  if (!sideband.SourceIsZeroWidthInsertion())
    return false;

  const auto sidebandB = sideband.MaterializedBByteRange();
  return patchBByteBegin <= sidebandB.first &&
         sidebandB.second <= patchBByteEnd;
}

} // namespace refold
} // namespace clang

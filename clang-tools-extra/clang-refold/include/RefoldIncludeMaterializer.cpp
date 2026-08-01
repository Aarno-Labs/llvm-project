//===--- RefoldIncludeMaterializer.cpp --------------------------*- C++ -*-===//
//
// Include materialization and include-owned edit planning.
//
// This file implements the service that realizes include subtrees, plans
// header-local edits, preserves include-owned sideband state, and assembles
// materialized include text from explicit proof and text-layout dependencies.
//
//===----------------------------------------------------------------------===//

#include "include/RefoldIncludeMaterializer.h"

#include "core/RefoldLog.h"
#include "edit/RefoldSourceEnvelopeTiling.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/IncludeSpellingHelpers.h"
#include "include/RefoldHeaderIncludeEditPlanner.h"
#include "include/RefoldIncludeInsertionPlanner.h"
#include "include/RefoldIncludeReplayProof.h"
#include "include/RefoldIncludeSubtreeWorkClassifier.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineControlEditHelpers.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "line-control/SourceLineDirectiveHelpers.h"
#include "macro/RefoldMacroStateProof.h"
#include "proof/RefoldNeutralityProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "util/RefoldDenseMapInfo.h"

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
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Adapt staged header TextEdits into the narrow interval-only surface needed
/// by RefoldMacroStateProof.  Macro-state stabilization only has to reject
/// overlapping local edits; it must not depend on edit payloads, accepted
/// carriers, or text-assembly metadata.
static SmallVector<MacroStateStagedEditInterval, 8>
macroStateStagedEditIntervals(
    ArrayRef<RefoldIncludeMaterializer::TextEdit> edits) {
  SmallVector<MacroStateStagedEditInterval, 8> intervals;
  intervals.reserve(edits.size());
  for (const RefoldIncludeMaterializer::TextEdit &edit : edits)
    intervals.push_back(MacroStateStagedEditInterval{edit.start, edit.end});
  return intervals;
}

static bool isIncludeDirectiveHorizontalWhitespace(char c) {
  return c == '\r' || stringutils::isNonNewlineWs(c);
}

/// Adapter for include replay proof source slicing.
/// The callable borrows the source mapper for the proof context lifetime.
struct IncludeReplaySliceASource {
  /// Source-mapping service used to slice the original A source.
  const RefoldSourceMapper &sourceMapper_;

  /// Returns the A-source slice for the requested byte interval.
  StringRef operator()(uint64_t begin, uint64_t end) const {
    return sourceMapper_.SliceASource(begin, end);
  }
};

/// Adapter for include replay proof physical-file comparison.
/// The callable keeps path identity policy outside the proof layer.
struct IncludeReplaySamePhysicalIncludeFile {
  /// Path-identity service used to compare include-selected files.
  const RefoldPathIdentity &paths_;

  /// Returns whether the candidate path names the same file as the include.
  bool operator()(StringRef candidatePath,
                  const RefoldModel::IncludeItem &include) const {
    return paths_.SamePhysicalIncludeFile(candidatePath, include);
  }
};

/// Adapter for macro sites that can observe line-state changes.
/// The callable exposes only the proof-layer query needed by include replay.
struct IncludeReplayLineStateObservableMacroSite {
  /// Line-control proof service used for observer queries.
  const RefoldLineControlProof &lineControlProof_;

  /// Returns the observable macro site, or null when none is relevant.
  const RefoldModel::MacroInvocation *
  operator()(const RefoldModel::MacroInvocation &macro) const {
    return lineControlProof_.LineStateObservableMacroSite(macro);
  }
};

/// Adapter for preserved builtin line-state observers.
/// The callable forwards the narrow observer-preservation predicate.
struct IncludeReplayLineStateBuiltinInvocationIsPreservedObserver {
  /// Line-control proof service used for builtin-observer preservation.
  const RefoldLineControlProof &lineControlProof_;

  /// Returns whether the builtin invocation's line-state observation survives.
  bool operator()(const RefoldModel::MacroInvocation &macro) const {
    return lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(
        macro);
  }
};

/// Adapter for include-subtree line-state observer demand.
/// The callable keeps demand classification scoped to the proof invocation.
struct IncludeReplayIncludeSubtreeLineStateObserverDemand {
  /// Line-control proof service used for subtree demand classification.
  const RefoldLineControlProof &lineControlProof_;

  /// Returns the observer demand required by the include subtree.
  LineStateObserverDemand operator()(uint64_t includeId) const {
    return lineControlProof_.IncludeSubtreeLineStateObserverDemand(includeId);
  }
};

struct IncludeDirectiveHeaderOperandRange {
  size_t begin = 0;
  size_t end = 0;
};

// Locate pieces of a source-spelled include directive without rebuilding the
// directive from the normalized JSON spelling.  Comments are preprocessing
// whitespace for directive recognition, so they must be skipped while finding
// the syntactic header-name token, but preserved verbatim in the replacement
// text.
static bool consumeIncludeDirectiveEscapedNewline(StringRef text, size_t &pos) {
  if (pos >= text.size() || text[pos] != '\\')
    return false;

  size_t cursor = pos + 1;
  while (cursor < text.size() &&
         isIncludeDirectiveHorizontalWhitespace(text[cursor]))
    ++cursor;
  if (cursor >= text.size())
    return false;
  if (text[cursor] == '\r') {
    ++cursor;
    if (cursor < text.size() && text[cursor] == '\n')
      ++cursor;
  } else if (text[cursor] == '\n') {
    ++cursor;
  } else {
    return false;
  }

  pos = cursor;
  return true;
}

static bool skipIncludeDirectiveHorizontalTrivia(StringRef text, size_t &pos) {
  while (pos < text.size()) {
    if (isIncludeDirectiveHorizontalWhitespace(text[pos])) {
      ++pos;
      continue;
    }
    if (consumeIncludeDirectiveEscapedNewline(text, pos))
      continue;
    if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '*') {
      pos += 2;
      bool closed = false;
      while (pos + 1 < text.size()) {
        if (consumeIncludeDirectiveEscapedNewline(text, pos))
          continue;
        if (text[pos] == '*' && text[pos + 1] == '/') {
          pos += 2;
          closed = true;
          break;
        }
        ++pos;
      }
      if (!closed)
        return false;
      continue;
    }
    if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '/')
      return false;
    break;
  }
  return true;
}

static bool consumeIncludeDirectiveKeyword(StringRef text, size_t &pos,
                                           StringRef keyword) {
  if (!text.substr(pos).starts_with(keyword))
    return false;
  const size_t end = pos + keyword.size();
  if (end < text.size() &&
      (stringutils::isIdentPart(text[end]) || text[end] == '_'))
    return false;
  pos = end;
  return true;
}

static std::optional<std::pair<size_t, size_t>>
findIncludeDirectiveKeywordRange(StringRef directive) {
  size_t pos = 0;
  if (!skipIncludeDirectiveHorizontalTrivia(directive, pos))
    return std::nullopt;
  if (pos >= directive.size() || directive[pos] != '#')
    return std::nullopt;
  ++pos;
  if (!skipIncludeDirectiveHorizontalTrivia(directive, pos))
    return std::nullopt;

  const size_t keywordBegin = pos;
  if (consumeIncludeDirectiveKeyword(directive, pos, "include_next"))
    return std::make_pair(keywordBegin, pos);
  pos = keywordBegin;
  if (consumeIncludeDirectiveKeyword(directive, pos, "include"))
    return std::make_pair(keywordBegin, pos);
  return std::nullopt;
}

// Return the exact byte range of the directive's syntactic header-name token.
// `expectedTarget` includes its delimiters, e.g. `"leaf.h"` or `<leaf.h>`.
// If the source uses a macro operand or any spelling the local proof does not
// model exactly, the caller fails closed and materializes the child instead.
static std::optional<IncludeDirectiveHeaderOperandRange>
findIncludeDirectiveHeaderOperandRange(StringRef directive,
                                       StringRef expectedTarget) {
  size_t pos = 0;
  if (!skipIncludeDirectiveHorizontalTrivia(directive, pos))
    return std::nullopt;
  if (pos >= directive.size() || directive[pos] != '#')
    return std::nullopt;
  ++pos;
  if (!skipIncludeDirectiveHorizontalTrivia(directive, pos))
    return std::nullopt;

  if (!consumeIncludeDirectiveKeyword(directive, pos, "include_next")) {
    if (!consumeIncludeDirectiveKeyword(directive, pos, "include"))
      return std::nullopt;
  }

  if (!skipIncludeDirectiveHorizontalTrivia(directive, pos))
    return std::nullopt;
  if (expectedTarget.empty())
    return std::nullopt;
  if (!directive.substr(pos).starts_with(expectedTarget))
    return std::nullopt;

  return IncludeDirectiveHeaderOperandRange{pos, pos + expectedTarget.size()};
}

/// Extends an include directive span through any physical line continuations.
/// The returned end offset covers the complete directive line in `ownerBytes`.
static uint64_t extendIncludeDirectiveEnd(const RefoldModel::IncludeItem &item,
                                          StringRef ownerBytes,
                                          uint64_t siteStart) {
  uint64_t siteEnd =
      std::clamp<uint64_t>(item.siteE, siteStart, ownerBytes.size());
  if (siteStart >= static_cast<uint64_t>(ownerBytes.size()))
    return siteEnd;

  size_t cursor = static_cast<size_t>(siteStart);
  while (true) {
    size_t nl = ownerBytes.find('\n', cursor);
    if (nl == StringRef::npos)
      return static_cast<uint64_t>(ownerBytes.size());
    cursor = nl + 1;
    if (!stringutils::isLineSplice(ownerBytes, nl))
      return std::max<uint64_t>(siteEnd, cursor);
  }
}

/// Rewrites a leading `#include_next` directive keyword to ordinary `#include`.
/// Returns whether the replacement text was changed.
static bool
rewriteIncludeNextDirectiveAsOrdinaryInclude(std::string &replacement) {
  StringRef text(replacement);
  std::optional<std::pair<size_t, size_t>> keyword =
      findIncludeDirectiveKeywordRange(text);
  if (!keyword)
    return false;

  StringRef spelling = text.slice(keyword->first, keyword->second);
  if (spelling != "include_next")
    return false;

  replacement.replace(keyword->first, keyword->second - keyword->first,
                      "include");
  return true;
}

/// Orders text edits by source interval start, then end. This is used for
/// deterministic traversal of edit pointers.
static bool
textEditPointerPrecedes(const RefoldIncludeMaterializer::TextEdit *lhs,
                        const RefoldIncludeMaterializer::TextEdit *rhs) {
  if (lhs->start != rhs->start)
    return lhs->start < rhs->start;
  return lhs->end < rhs->end;
}

/// Computes the first physical header line that survives staged edits.
/// Leading deletions advance the reported line, while leading replacements
/// still occupy their original source line.
static size_t computeFirstEmittedHeaderLine(
    StringRef bytes, ArrayRef<RefoldIncludeMaterializer::TextEdit> edits) {
  if (edits.empty())
    return 1;

  SmallVector<const RefoldIncludeMaterializer::TextEdit *, 8> ordered;
  ordered.reserve(edits.size());
  for (const RefoldIncludeMaterializer::TextEdit &edit : edits)
    ordered.push_back(&edit);
  llvm::sort(ordered, textEditPointerPrecedes);

  // Track the first physical line that will actually be emitted from the
  // materialized header.  A leading sideband deletion consumes the directive
  // line before wrapping, so the include-enter #line must name the first
  // surviving source line, not blindly line 1 of the header.  A leading
  // replacement still occupies the replaced directive's logical line.
  uint64_t cursor = 0;
  for (const RefoldIncludeMaterializer::TextEdit *edit : ordered) {
    if (cursor < edit->start)
      return stringutils::lineAtOffset(bytes, cursor);
    if (cursor > edit->start)
      continue;
    if (!edit->text.empty())
      return stringutils::lineAtOffset(bytes, edit->start);
    cursor = std::max(cursor, edit->end);
  }

  if (cursor < bytes.size())
    return stringutils::lineAtOffset(bytes, cursor);
  return 1;
}

} // namespace

std::optional<std::string>
RefoldIncludeMaterializer::BuildInlineIncludeRealizationFromB(
    const RefoldModel::IncludeItem &inc, StringRef reason,
    AcceptedResultCandidate *acceptedCandidate) const {
  // Inline include realization is theorem-facing only when the include's
  // A-cover can be projected to a concrete B-token envelope by an accepted
  // witness: either the canonical A-cover mapping or the boundary-stable
  // consensus proof.  If no such envelope exists, do not synthesize a weaker
  // realization; request the explicit terminal fallback instead.
  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvOpt =
      includeInsertionPlanner_.ResolveIncludeRealizationBTokenEnvelope(
          inc.cover.begin, inc.cover.end, &evidenceKind);
  if (!bEnvOpt) {
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::IncludeRealizationBEnvelopeMapped,
            TerminalFallbackFailureReason::UnmappableIncludeBEnvelope),
        "include/mat",
        llvm::formatv("include realization from B failed to resolve a "
                      "canonical-or-consensus-resolvable B envelope for "
                      "A cover [{0},{1}) for inc#{2}; reason={3}",
                      inc.cover.begin, inc.cover.end, inc.id, reason)
            .str());
    return std::nullopt;
  }

  // Build the normalized accepted-result carrier before returning the sliced
  // text.  The producer-proven B envelope is passed directly into the shared
  // owner-realization proof path instead of being copied into a parallel
  // include-specific closure witness.
  AcceptedResultCandidate realizationCandidate =
      proofLattice_.AcceptedCandidateBuilder()
          .BuildAcceptedIncludeRealizationCandidate(
              AcceptedPathKind::IncludeRealizationInlineFromB, inc,
              evidenceKind, *bEnvOpt);
  if (acceptedCandidate)
    *acceptedCandidate = realizationCandidate;

  return sourceMapper_.SliceBSource(bEnvOpt->first, bEnvOpt->second).str();
}

void RefoldIncludeMaterializer::MaterializeIncludeExpansion(
    uint64_t includeId, const DenseMap<uint64_t, IncludeEdits> &perInclude,
    const DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
        &macroPatchesByOwner,
    const DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
        &children,
    DenseMap<uint64_t, std::string> &includeExpansion,
    DenseMap<uint64_t, std::vector<FinalLineControlPruneCandidate>>
        &includeExpansionLineControlPruneCandidates,
    DenseMap<uint64_t, std::vector<FinalLineControlSourceMapping>>
        &includeExpansionLineControlSourceMappings,
    DenseMap<uint64_t, size_t> &includeExpansionStartLineNos,
    DenseMap<uint64_t, AcceptedResultCandidate>
        &includeExpansionAcceptedResults,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds,
    bool materializeIncludeNextInThisSubtree) const {
  // Include materialization is memoized by include id. A parent may reach the
  // same child through recursive expansion, so avoid rebuilding already
  // materialized text.
  if (includeExpansion.count(includeId)) {
    // Preseeded or previously materialized include bodies that do not carry a
    // more precise source-line witness still enter at the header start.
    if (!includeExpansionStartLineNos.count(includeId))
      includeExpansionStartLineNos[includeId] = 1;
    return;
  }

  const RefoldModel::IncludeItem *inc = model_.GetIncludeById(includeId);
  if (!inc)
    REFOLD_LOG_FATAL("include/mat", "unknown includeId {0}", includeId);

  const std::string headerPath = refoldIncludeEnteredFileSpelling(*inc);
  const std::string headerLoadPath = refoldIncludeLoadPath(*inc);

  // Start from the header's original source bytes. Some callers preseed
  // `includeExpansion` with raw header text; otherwise load it
  // deterministically from the producer-selected physical file.  The logical
  // headerPath above is reserved for owner matching and `#line` spelling
  // repair.
  std::string bytes;
  if (auto it = includeExpansion.find(includeId); it != includeExpansion.end())
    bytes = it->second;
  if (bytes.empty()) {
    auto bufOrErr =
        MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(headerLoadPath));
    if (!bufOrErr) {
      REFOLD_LOG_FATAL(
          "include/mat", "failed to read header: {0} (load path {1}) ({2})",
          headerPath, headerLoadPath, bufOrErr.getError().message());
    }

    const MemoryBuffer &mb = **bufOrErr;
    bytes.assign(mb.getBufferStart(), mb.getBufferEnd());
  }

  auto editsIt = perInclude.find(includeId);
  // Accumulate all local edits in this header byte space, then apply them once
  // at the end. This lets include edits, macro patches, and child include
  // materializations participate in one overlap/resync pass.
  std::vector<TextEdit> edits;

  if (std::optional<std::string> realizationReason =
          MaterializedHeaderRequiresBRealizationReason(*inc, headerPath,
                                                       headerLoadPath)) {
    (void)TryRecordInlineIncludeRealizationFromB(
        *inc, *realizationReason, includeExpansion,
        includeExpansionStartLineNos, includeExpansionAcceptedResults);
    return;
  }

  auto includeInsertionPatchCarriesSidebandReplay =
      [&](const SidebandPragmaEdit &sideband) -> bool {
    if (editsIt == perInclude.end())
      return false;
    if (!sideband.SourceIsZeroWidthInsertion())
      return false;

    for (const IncludePatch &patch : editsIt->second.patches) {
      if (patch.aStart != patch.aEnd || patch.bStart >= patch.bEnd)
        continue;
      if (patch.bStart >= bTokOff_.size() || patch.bEnd > bTokOff_.size())
        continue;

      StringRef patchEnvelope = refoldSliceTokenEnvelope(
          bTokOff_, bSource_, patch.bStart, patch.bEnd);
      const uint64_t patchB = bTokOff_[static_cast<size_t>(patch.bStart)];
      const uint64_t patchE = patchB + patchEnvelope.size();
      if (sidebandInsertionReplayIsCoveredByPatchEnvelope(sideband, patchB,
                                                          patchE))
        return true;
    }
    return false;
  };

  // Sideband pragma edits owned by this include were printed verbatim in the
  // raw `.i` replay surface but deliberately removed from the normal A/B token
  // streams before diffing. When such a pragma lives in a header, deleting or
  // replacing it is an include-owned source edit, not a TU edit. Queue it here
  // so it composes with the same owner-polymorphic header materialization path
  // used for ordinary include and macro edits.
  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits_) {
    if (!sideband.TargetsInclude(includeId))
      continue;

    const auto sourceRange = sideband.SourceByteRange();
    if (!validateAndReportSidebandPragmaEditProof(
            sideband, static_cast<uint64_t>(bSource_.size()), terminalSink_,
            "pragma/sideband/include", /*traceSuccess=*/false)) {
      includeExpansion[includeId] = std::string();
      return;
    }

    if (!sideband.SourceIsWithinOwnerBytes(
            static_cast<uint64_t>(bytes.size()))) {
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::PragmaBoundaryKnown,
              TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary),
          "pragma/sideband",
          llvm::formatv("header-owned sideband pragma edit has invalid range "
                        "inc#{0} path='{1}' site=[{2},{3}) headerSize={4}",
                        includeId, headerPath, sourceRange.first,
                        sourceRange.second, bytes.size())
              .str());
      includeExpansion[includeId] = std::string();
      return;
    }

    if (includeInsertionPatchCarriesSidebandReplay(sideband)) {
      // A header-prefix/suffix ordinary insertion patch uses the full B token
      // envelope.  If that envelope already contains this sideband replay
      // range, staging a second header-owned sideband edit would duplicate the
      // pragma in the materialized header.  The sideband proof is still
      // necessary for owner selection and token-stream normalization, but the
      // include patch is the unique replay owner for the visible bytes.
      continue;
    }

    auto hasCopiedHeaderSuffixAfterSidebandEdit = [&]() -> bool {
      // A header-local resync can only protect bytes that remain in this same
      // materialized header after the sideband edit.  If the sideband edit is
      // the last visible header-owned replay text, the parent-resume wrapper is
      // the next meaningful line-state transition and an extra local #line
      // would be gratuitous.
      for (uint64_t p = sourceRange.second; p < bytes.size(); ++p)
        if (!stringutils::isWs(bytes[p]))
          return true;
      return false;
    };

    // A sideband pragma line is not part of the normal PP token stream, but it
    // can still be visible header-owned replay text.  The include enter/exit
    // wrapper handles the owner transition for that visible replay.
    // Header-local resync is narrower: emit it only when sideband-owned
    // trailing B whitespace would otherwise shift copied header suffix bytes,
    // or when the suffix has a source-location observer such as
    // __LINE__/__FILE__.
    const bool mustPreserveHeaderLineState =
        (sideband.ForcesIncludeLineDirectiveWrappers() &&
         hasCopiedHeaderSuffixAfterSidebandEdit() &&
         sideband.OwnsTrailingReplayBlankLine()) ||
        lineControlProof_.OwnerSuffixHasLineStateSensitiveBuiltin(
            includeId, headerPath, sourceRange.second);
    ResyncOutcome ro =
        mustPreserveHeaderLineState
            ? textEditAssembler_.ApplyResyncOrPend(
                  bytes, sourceRange.first, sourceRange.second,
                  sideband.ReplacementText(), headerPath, includeId)
            : ResyncOutcome(sideband.ReplacementText().str(), std::nullopt);
    TextEdit edit{sourceRange.first,
                  sourceRange.second,
                  std::move(ro.text),
                  std::move(ro.pending),
                  std::nullopt,
                  {},
                  {},
                  {}};
    edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);
    const PreprocessingStructureKind pragmaKinds[] = {
        PreprocessingStructureKind::Pragma,
        PreprocessingStructureKind::PragmaOperator};
    if (!textEditAssembler_.AuthorizeProtectedSourceIntervals(
            edit, ProtectedSourceEditAuthorityKind::SidebandPragmaEdit,
            headerPath, includeId, bytes, sourceRange.first,
            sourceRange.second, pragmaKinds,
            /*requireProtectedInterval=*/false)) {
      includeExpansion[includeId] = std::string();
      return;
    }
    textEditAssembler_.AttachAcceptedResultCarrier(
        edit, proofLattice_.AcceptedCandidateBuilder()
                  .BuildAcceptedIncludeRealizationCandidate(
                      AcceptedPathKind::IncludeMaterializedExpansion, *inc));
    edits.push_back(std::move(edit));
  }

  // The rest of this method assembles the header's TextEdit list in three
  // ordered passes.  Macro-replay-context stability planning is delegated
  // to `RefoldMacroStateProof`; the mechanical edit construction,
  // local line-resync, accepted-result attachment, and child-include
  // recursion control flow all stay here.

  // 1) Macro patches owned by this include. Macro invocation ranges are already
  // recorded in the owner file's byte space, so they can be converted directly
  // into local header TextEdits.  Before staging the TextEdit, prove replay
  // context stability for the B-surface text: if the replacement would observe
  // an active header-owned #define that B did not have active, move that
  // definition after the completed replacement line.  Macro-state movement is
  // admitted only when every crossed source byte is proven non-observing;
  // otherwise materialization fails closed instead of emitting a refolding that
  // preprocesses under the wrong macro environment.
  if (auto it = macroPatchesByOwner.find(includeId);
      it != macroPatchesByOwner.end()) {
    for (const auto &mp : it->second) {
      uint64_t mpEnd = stringutils::extendChainedCallEnd(
          StringRef(bytes), mp.invRange.end, mp.replacement);
      uint64_t editStart = mp.invRange.begin;
      uint64_t editEnd = mpEnd;
      std::string editReplacement = mp.replacement;
      std::vector<MacroStateDirectiveLineInterval> movedMacroStateTransitions;
      if (std::optional<StabilizedMaterializedHeaderMacroPatch> stabilized =
              macroStateProof_.StabilizeMaterializedHeaderMacroPatchReplay(
                  MacroStatePatchReplayInput{mp.invRange.begin, mp.invRange.end,
                                             StringRef(mp.replacement)},
                  mpEnd, headerPath, includeId, StringRef(bytes),
                  macroStateStagedEditIntervals(edits))) {
        // Empty {0,0,""} is the local sentinel used by the stabilizer after
        // it has requested terminal fallback.  Stop materializing this include
        // so no partially proven header edit is emitted.
        if (stabilized->replacement.empty() && stabilized->start == 0 &&
            stabilized->end == 0) {
          includeExpansion[includeId] = std::string();
          return;
        }
        editStart = stabilized->start;
        editEnd = stabilized->end;
        editReplacement = std::move(stabilized->replacement);
        movedMacroStateTransitions =
            std::move(stabilized->movedTransitions);
      }

      // Apply local line-resync policy before staging the edit, and carry the
      // accepted macro proof metadata to the emitted TextEdit.
      ResyncOutcome ro = textEditAssembler_.ApplyResyncOrPend(
          bytes, editStart, editEnd, editReplacement, headerPath, includeId);
      TextEdit edit{
          editStart,
          editEnd,
          std::move(ro.text),
          std::move(ro.pending),
          macroTopology_.MacroPatchRemainsExpanded(mp)
              ? std::make_optional(macroTopology_.GetRootMacroId(mp.macroId))
              : std::nullopt,
          {},
          {},
          {}};
      edit.lineControlPruneCandidates =
          std::move(ro.lineControlPruneCandidates);
      if (!movedMacroStateTransitions.empty()) {
        const PreprocessingStructureKind macroStateKinds[] = {
            PreprocessingStructureKind::MacroDefine,
            PreprocessingStructureKind::MacroUndef};
        for (const MacroStateDirectiveLineInterval &transition :
             movedMacroStateTransitions) {
          if (textEditAssembler_.AuthorizeExactProtectedSourceInterval(
                  edit, ProtectedSourceEditAuthorityKind::MacroStateRepair,
                  headerPath, includeId, bytes, transition.begin,
                  transition.end, macroStateKinds)) {
            continue;
          }
          includeExpansion[includeId] = std::string();
          return;
        }
      }
      textEditAssembler_.AttachAcceptedResultCarrier(
          edit, proofLattice_.AcceptedCandidateBuilder()
                    .BuildAcceptedEmittedMacroCandidate(mp));
      edits.push_back(std::move(edit));
    }
  }

  // 2) Include-preserving edits owned by this include. If those edits cannot be
  // discharged as local anchored edits, materialize the whole include directly
  // from B using the include-realization proof path.
  if (auto it = perInclude.find(includeId); it != perInclude.end()) {
    if (!it->second.patches.empty()) {

      IncludeTextEditPlan plan = ComputeIncludeTextEdits(it->second, bytes);
      if (plan.requiresIncludeRealization) {
        AcceptedResultCandidate realizationCandidate;
        if (auto realized = BuildInlineIncludeRealizationFromB(
                *inc, plan.realizationReason, &realizationCandidate)) {
          includeExpansion[includeId] = std::move(*realized);
          includeExpansionStartLineNos[includeId] = 1;
          includeExpansionAcceptedResults[includeId] =
              std::move(realizationCandidate);
          return;
        }
        includeExpansion[includeId] = std::string();
        return;
      }

      for (auto &te : plan.edits) {
        // ComputeIncludeTextEdits() already applies the owner-local padding and
        // line-resync proof for each header edit.  Re-running resync here would
        // treat a deliberately inserted `#line` as fresh newline drift and can
        // duplicate the same local resume before the copied suffix.
        edits.push_back(std::move(te));
      }
    }
  }

  using MaterializationWorkClass =
      RefoldIncludeSubtreeWorkClassifier::MaterializationWorkClass;
  RefoldIncludeSubtreeWorkClassifier subtreeWork(
      model_, perInclude, macroPatchesByOwner, children, sidebandPragmaEdits_);

  // Include replay proof is read-only.  The function_ref services borrow
  // named adapter objects whose storage outlives the short-lived proof context.
  IncludeReplaySliceASource includeReplaySliceASource{sourceMapper_};
  IncludeReplaySamePhysicalIncludeFile includeReplaySamePhysicalIncludeFile{
      paths_};
  IncludeReplayLineStateObservableMacroSite
      includeReplayLineStateObservableMacroSite{lineControlProof_};
  IncludeReplayLineStateBuiltinInvocationIsPreservedObserver
      includeReplayLineStateBuiltinInvocationIsPreservedObserver{
          lineControlProof_};
  IncludeReplayIncludeSubtreeLineStateObserverDemand
      includeReplayIncludeSubtreeLineStateObserverDemand{lineControlProof_};

  IncludeReplayProofInputs includeReplayProofInputs{model_, aSource_, lineDirs_,
                                                    finalReplaySurface_};
  IncludeReplayProofServices includeReplayProofServices{
      includeReplaySliceASource, includeReplaySamePhysicalIncludeFile,
      includeReplayLineStateObservableMacroSite,
      includeReplayLineStateBuiltinInvocationIsPreservedObserver,
      includeReplayIncludeSubtreeLineStateObserverDemand};
  IncludeReplayProofContext includeReplayProof(includeReplayProofInputs,
                                               includeReplayProofServices);

  // 3) Materialize child includes only when their subtree has work. For each
  // dirty child, recursively build the child's fully materialized text and then
  // replace the child's include directive in this header.
  if (auto it = children.find(includeId); it != children.end()) {
    for (const auto *child : it->second) {
      bool todo = subtreeWork.HasDescendantWork(child->id);

      // Forced include-next materialization is subtree-wide, not
      // direct-child-only.  The bug class this machinery protects against is a
      // materialized header that preserves a clean ordinary child include whose
      // nested #include_next then resumes lookup from the wrong HeaderSearch
      // cursor.  Force materialization along any path that can reach
      // #include_next so the directive itself is eventually replaced by the
      // producer-selected subtree unless a normal, local proof has already kept
      // us out of this forced mode.
      if (materializeIncludeNextInThisSubtree &&
          subtreeWork.SubtreeContainsIncludeNext(child->id))
        todo = true;

      IncludeReplayProofContext::CleanChildIncludeReplayPlan replayPlan;
      if (!todo) {
        replayPlan =
            includeReplayProof
                .PlanCleanChildIncludeReplayFromMaterializedParent(*child);
        if (replayPlan.action ==
            IncludeReplayProofContext::CleanChildIncludeReplayAction::
                RewriteOperand) {
          if (std::optional<TextEdit> rewrite =
                  MakeCleanChildIncludeOperandRewriteEdit(
                      *child, replayPlan.rewrittenOperand,
                      replayPlan.rewrittenDelimiterKind, bytes)) {
            REFOLD_LOG_TRACE(
                "include/lookup",
                "rewrite clean child include #{0} for materialized-parent "
                "lookup: target={1} delimiter={2} rewritten={3}{4}{5} "
                "reason={6}",
                child->id, child->target,
                IncludeReplayProofContext::OrdinaryIncludeDelimiterName(
                    replayPlan.rewrittenDelimiterKind),
                IncludeReplayProofContext::OrdinaryIncludeDelimiterOpen(
                    replayPlan.rewrittenDelimiterKind),
                replayPlan.rewrittenOperand,
                IncludeReplayProofContext::OrdinaryIncludeDelimiterClose(
                    replayPlan.rewrittenDelimiterKind),
                replayPlan.reason);
            edits.push_back(std::move(*rewrite));
          } else {
            // The lookup proof was for preserving this child as an include
            // directive with a specific rewritten ordinary operand.  If the
            // parent-surface source edit cannot be formed, that proof is no
            // longer usable: materializing the child may relocate any nested
            // #include_next directives into the flattened header text.  Carry a
            // subtree-wide force flag in that case so recursive materialization
            // continues down to include-next edges instead of accidentally
            // leaving a replay-stack-sensitive directive behind.
            REFOLD_LOG_TRACE(
                "include/lookup",
                "failed to rewrite clean child include #{0}; materializing "
                "child edge instead",
                child->id);
            replayPlan.action = IncludeReplayProofContext::
                CleanChildIncludeReplayAction::Materialize;
            if (subtreeWork.SubtreeContainsIncludeNext(child->id))
              replayPlan.forceMaterializeDescendantIncludeNext = true;
          }
        }

        if (replayPlan.action !=
            IncludeReplayProofContext::CleanChildIncludeReplayAction::
                Materialize) {
          // No descendant edits depend on this child.  Either the original
          // directive was lookup-stable and can stay as-is, or we staged a
          // local operand rewrite above.
          continue;
        }
      }

      const bool forceIncludeNextMaterialization =
          materializeIncludeNextInThisSubtree ||
          replayPlan.forceMaterializeDescendantIncludeNext;

      // Depth-first materialization ensures the child replacement is already
      // complete before it is wrapped and inserted into the parent header.
      MaterializeIncludeExpansion(
          child->id, perInclude, macroPatchesByOwner, children,
          includeExpansion, includeExpansionLineControlPruneCandidates,
          includeExpansionLineControlSourceMappings,
          includeExpansionStartLineNos, includeExpansionAcceptedResults,
          appliedExpandedMacroRootIds, forceIncludeNextMaterialization);

      const auto &childText = includeExpansion[child->id];
      const size_t n = bytes.size();
      const uint64_t siteStart = std::clamp<uint64_t>(child->siteB, 0ULL, n);
      uint64_t siteEnd = extendIncludeDirectiveEnd(*child, bytes, siteStart);

      if (siteStart < siteEnd) {
        const bool sidebandOnly =
            subtreeWork.ClassifyMaterializationWork(child->id) ==
            MaterializationWorkClass::SidebandPragmaOnly;
        const size_t childEntryLineNo =
            includeExpansionStartLineNos.lookup(child->id);
        LineDirectiveLocation parentResume =
            LineDirectiveInserter::LogicalLocationAtOffset(
                bytes, siteEnd, headerPath, model_, headerPath, includeId);
        ArrayRef<FinalLineControlPruneCandidate> childLineCandidates;
        if (auto childCandidatesIt =
                includeExpansionLineControlPruneCandidates.find(child->id);
            childCandidatesIt !=
            includeExpansionLineControlPruneCandidates.end())
          childLineCandidates = childCandidatesIt->second;
        ArrayRef<FinalLineControlSourceMapping> childLineSourceMappings;
        if (auto childMappingsIt =
                includeExpansionLineControlSourceMappings.find(child->id);
            childMappingsIt != includeExpansionLineControlSourceMappings.end())
          childLineSourceMappings = childMappingsIt->second;
        LineControlWrappedText wrapped =
            lineObserverLayout_.WrapIncludeExpansionForMaterialization(
                *child, parentResume.fileSpelling, headerPath, includeId,
                siteEnd, childEntryLineNo ? childEntryLineNo : 1,
                parentResume.lineNo, childText, childLineCandidates,
                childLineSourceMappings, sidebandOnly);
        TextEdit edit{siteStart,
                      siteEnd,
                      std::move(wrapped.text),
                      std::nullopt,
                      std::nullopt,
                      {},
                      {},
                      {}};
        edit.lineControlPruneCandidates =
            std::move(wrapped.lineControlPruneCandidates);
        edit.lineControlSourceMappings =
            std::move(wrapped.lineControlSourceMappings);
        if (sidebandOnly ||
            subtreeWork.UsesOnlySidebandReplayEnvelope(child->id)) {
          if (auto sidebandBRange =
                  textEditAssembler_
                      .SidebandPragmaMaterializedBByteRangeForInclude(
                          child->id))
            textEditAssembler_.CertifyTextEditMaterializedBByteRange(
                edit, sidebandBRange->first, sidebandBRange->second);
        }

        const PreprocessingStructureKind includeKinds[] = {
            PreprocessingStructureKind::Include,
            PreprocessingStructureKind::IncludeNext,
            PreprocessingStructureKind::Import};
        if (!textEditAssembler_.AuthorizeProtectedSourceIntervals(
                edit,
                ProtectedSourceEditAuthorityKind::IncludeMaterialization,
                headerPath, includeId, bytes, siteStart, siteEnd,
                includeKinds)) {
          includeExpansion[includeId] = std::string();
          return;
        }

        // Preserve the child's accepted-result carrier if the child was
        // materialized through a specific realization path; otherwise record
        // the generic materialized-expansion carrier for this child include.
        auto itAccepted = includeExpansionAcceptedResults.find(child->id);
        if (itAccepted != includeExpansionAcceptedResults.end()) {
          textEditAssembler_.AttachAcceptedResultCarrier(edit,
                                                         itAccepted->second);
        } else {
          textEditAssembler_.AttachAcceptedResultCarrier(
              edit,
              proofLattice_.AcceptedCandidateBuilder()
                  .BuildAcceptedIncludeRealizationCandidate(
                      AcceptedPathKind::IncludeMaterializedExpansion, *child));
        }
        edits.push_back(std::move(edit));
      } else {
        // A degenerate child site cannot discharge an include-preserving edit
        // plan for the parent. Select the explicit include-realization proof
        // class for the whole parent include immediately.
        AcceptedResultCandidate realizationCandidate;
        if (auto realized = BuildInlineIncludeRealizationFromB(
                *inc,
                llvm::formatv("degenerate child replace site for parent inc#{0}"
                              " child#{1} site=[{2},{3})",
                              inc->id, child->id, siteStart, siteEnd)
                    .str(),
                &realizationCandidate)) {
          includeExpansion[includeId] = std::move(*realized);
          includeExpansionStartLineNos[includeId] = 1;
          includeExpansionAcceptedResults[includeId] =
              std::move(realizationCandidate);
        } else {
          includeExpansion[includeId] = std::string();
        }
        return;
      }
    }
  }

  const size_t firstEmittedHeaderLine =
      computeFirstEmittedHeaderLine(bytes, edits);

  // Apply all staged edits in this header. If application requests terminal
  // fallback, leave this include empty so callers do not consume a partial
  // materialization.
  std::vector<FinalLineControlPruneCandidate> appliedLineControlCandidates;
  std::vector<FinalLineControlSourceMapping> appliedLineControlSourceMappings;
  std::string applied = textEditAssembler_.ApplyTextEditsWithPendingResync(
      bytes, edits, appliedExpandedMacroRootIds, headerPath, includeId,
      /*materializedEditMappings=*/nullptr, &appliedLineControlCandidates,
      &appliedLineControlSourceMappings);
  if (terminalSink_.HasRequest()) {
    includeExpansion[includeId] = std::string();
    includeExpansionLineControlPruneCandidates[includeId].clear();
    includeExpansionLineControlSourceMappings[includeId].clear();
    return;
  }

  includeExpansion[includeId] = std::move(applied);
  deduplicateLineControlPruneCandidates(appliedLineControlCandidates);
  deduplicateLineControlSourceMappings(appliedLineControlSourceMappings);
  includeExpansionLineControlPruneCandidates[includeId] =
      std::move(appliedLineControlCandidates);
  includeExpansionLineControlSourceMappings[includeId] =
      std::move(appliedLineControlSourceMappings);
  includeExpansionStartLineNos[includeId] = firstEmittedHeaderLine;
  includeExpansionAcceptedResults[includeId] =
      proofLattice_.AcceptedCandidateBuilder()
          .BuildAcceptedIncludeRealizationCandidate(
              AcceptedPathKind::IncludeMaterializedExpansion, *inc);
}

bool RefoldIncludeMaterializer::PathNamesMaterializedHeader(
    StringRef path, StringRef headerPath, StringRef headerLoadPath) const {
  if (path.empty())
    return false;
  if (path == headerPath || path == headerLoadPath)
    return true;

  // Header-owner checks are hardening predicates, not the semantic filename
  // proof itself.  Use the non-fatal line-control path normalizer here rather
  // than canonicalizing the logical spelling: producer-entered spellings such
  // as `leaf.h` may only be meaningful inside Clang's search context, while
  // opened_path is the separately recorded readable file.
  if (!headerLoadPath.empty() &&
      lineControlProof_.SameLineControlPhysicalFile(path, headerLoadPath))
    return true;
  return lineControlProof_.SameLineControlPhysicalFile(path, headerPath);
}

std::optional<std::string>
RefoldIncludeMaterializer::MaterializedHeaderRequiresBRealizationReason(
    const RefoldModel::IncludeItem &include, StringRef headerPath,
    StringRef headerLoadPath) const {
  const uint64_t includeId = include.id;

  // A materialized header is replayed from the including surface, not from the
  // header's original file.  Conditional operators such as __has_include
  // perform quoted-header lookup relative to the file that contains the
  // directive; moving that directive out of its original header can therefore
  // flip the selected preprocessor branch even when ordinary macro state is
  // otherwise closed.
  for (const RefoldModel::CondGroup *group :
       model_.GetCondGroups(headerPath, includeId)) {
    if (!group)
      continue;
    for (const RefoldModel::CondArm &arm : group->arms) {
      if (!arm.cond)
        continue;
      if (arm.cond->contains("__has_include"))
        return "replay-context-sensitive conditional control in "
               "materialized header";
    }
  }

  // __INCLUDE_LEVEL__ observes include-stack depth, not line/file state.  Once
  // an include owner is materialized, the source-spelled header bytes are
  // replayed from the flattened parent/TU surface instead of being entered by
  // an actual #include edge.
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__INCLUDE_LEVEL__")
      continue;

    const RefoldModel::MacroInvocation *site =
        lineControlProof_.LineStateObservableMacroSite(macro);
    std::optional<uint64_t> owner = site && site->ownerIncludeId
                                        ? site->ownerIncludeId
                                        : macro.ownerIncludeId;
    if (!owner || *owner != includeId)
      continue;

    // The owner id is the semantic proof; the optional invocation file is a
    // hardening check against malformed producer records that accidentally
    // attach a builtin from another source buffer to this include instance.
    if (site && site->invFile &&
        !PathNamesMaterializedHeader(*site->invFile, headerPath,
                                     headerLoadPath))
      continue;
    if (macro.invFile &&
        !PathNamesMaterializedHeader(*macro.invFile, headerPath,
                                     headerLoadPath) &&
        (!site || !site->invFile ||
         !PathNamesMaterializedHeader(*site->invFile, headerPath,
                                      headerLoadPath)))
      continue;

    return "__INCLUDE_LEVEL__ observer in materialized header";
  }

  // `__FILE__` and `__FILE_NAME__` observe the current logical filename.  If
  // line directives are disabled, the only deterministic repair for preserved
  // source-spelled observers in a materialized header is the producer-proven B
  // realization, which literalizes the producer value.
  if (!lineDirs_.Enabled()) {
    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations()) {
      if (macro.name != "__FILE__" && macro.name != "__FILE_NAME__")
        continue;
      if (!lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(
              macro))
        continue;

      const RefoldModel::MacroInvocation *site =
          lineControlProof_.LineStateObservableMacroSite(macro);
      std::optional<uint64_t> owner = site && site->ownerIncludeId
                                          ? site->ownerIncludeId
                                          : macro.ownerIncludeId;
      if (!owner || *owner != includeId)
        continue;

      // The owner id is the semantic proof.  The file checks only reject
      // malformed records that accidentally attach a preserved builtin from
      // some other source buffer to this include instance.
      if (site && site->invFile &&
          !PathNamesMaterializedHeader(*site->invFile, headerPath,
                                       headerLoadPath))
        continue;
      if (macro.invFile &&
          !PathNamesMaterializedHeader(*macro.invFile, headerPath,
                                       headerLoadPath) &&
          (!site || !site->invFile ||
           !PathNamesMaterializedHeader(*site->invFile, headerPath,
                                        headerLoadPath)))
        continue;

      return "__FILE__/__FILE_NAME__ observer in materialized header without "
             "line repair";
    }
  }

  // With line directives enabled, materialization can preserve source-spelled
  // `__FILE__` / `__FILE_NAME__` only if the include edge has a producer
  // filename spelling to install before the copied body.  If the map lacks any
  // entered-file witness, do not invent one from the target token.
  if (lineDirs_.Enabled()) {
    const LineStateObserverDemand demand =
        lineControlProof_.IncludeSubtreeLineStateObserverDemand(includeId);
    if (demand.needsFile || demand.needsFileName) {
      StringRef producerSpelling = producerEnteredFileSpelling(include);
      if (producerSpelling.empty())
        return "__FILE__/__FILE_NAME__ observer in materialized header without "
               "producer entered-file spelling";

      if (demand.needsFileName && producerEnteredFileName(include).empty())
        return "__FILE__/__FILE_NAME__ observer in materialized header without "
               "producer entered-file spelling";
    }
  }

  // __BASE_FILE__ can be repaired by line directives, but in --no-lines mode a
  // materialized include owner has no way to preserve the source-spelled
  // observer without changing the top-level file it observes.
  if (!lineDirs_.Enabled()) {
    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations()) {
      if (macro.name != "__BASE_FILE__")
        continue;
      if (!lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(
              macro))
        continue;

      const RefoldModel::MacroInvocation *site =
          lineControlProof_.LineStateObservableMacroSite(macro);
      std::optional<uint64_t> owner = site && site->ownerIncludeId
                                          ? site->ownerIncludeId
                                          : macro.ownerIncludeId;
      if (!owner || *owner != includeId)
        continue;

      // The owner id is the semantic proof.  The file spelling checks are only
      // hardening against malformed producer records that accidentally attach a
      // builtin from another source buffer to this include instance.
      if (site && site->invFile &&
          !PathNamesMaterializedHeader(*site->invFile, headerPath,
                                       headerLoadPath))
        continue;
      if (macro.invFile &&
          !PathNamesMaterializedHeader(*macro.invFile, headerPath,
                                       headerLoadPath) &&
          (!site || !site->invFile ||
           !PathNamesMaterializedHeader(*site->invFile, headerPath,
                                        headerLoadPath)))
        continue;

      return "__BASE_FILE__ observer in materialized header without line "
             "repair";
    }
  }

  return std::nullopt;
}

bool RefoldIncludeMaterializer::TryRecordInlineIncludeRealizationFromB(
    const RefoldModel::IncludeItem &include, StringRef reason,
    DenseMap<uint64_t, std::string> &includeExpansion,
    DenseMap<uint64_t, size_t> &includeExpansionStartLineNos,
    DenseMap<uint64_t, AcceptedResultCandidate>
        &includeExpansionAcceptedResults) const {
  AcceptedResultCandidate realizationCandidate;
  if (auto realized = BuildInlineIncludeRealizationFromB(
          include, reason, &realizationCandidate)) {
    includeExpansion[include.id] = std::move(*realized);
    includeExpansionStartLineNos[include.id] = 1;
    includeExpansionAcceptedResults[include.id] =
        std::move(realizationCandidate);
    REFOLD_LOG_TRACE("include/mat", "realize include #{0} from B: {1}",
                     include.id, reason);
    return true;
  }

  includeExpansion[include.id] = std::string();
  return false;
}

std::optional<RefoldIncludeMaterializer::TextEdit>
RefoldIncludeMaterializer::MakeCleanChildIncludeOperandRewriteEdit(
    const RefoldModel::IncludeItem &child, StringRef rewrittenOperand,
    IncludeReplayProofContext::OrdinaryIncludeDelimiterKind delimiterKind,
    StringRef ownerBytes) const {
  const size_t n = ownerBytes.size();
  const uint64_t siteStart = std::clamp<uint64_t>(child.siteB, 0ULL, n);
  const uint64_t siteEnd =
      extendIncludeDirectiveEnd(child, ownerBytes, siteStart);
  if (siteStart >= siteEnd)
    return std::nullopt;
  if (!child.cover.IsValid() || child.cover.begin >= child.cover.end)
    return std::nullopt;

  StringRef directiveBytes = ownerBytes.slice(siteStart, siteEnd);
  std::optional<IncludeDirectiveHeaderOperandRange> targetRange =
      findIncludeDirectiveHeaderOperandRange(directiveBytes, child.target);
  if (!targetRange)
    return std::nullopt;

  std::string replacement = directiveBytes.str();
  std::string headerOperand;
  headerOperand.push_back(
      IncludeReplayProofContext::OrdinaryIncludeDelimiterOpen(delimiterKind));
  if (!rewrittenOperand.empty())
    headerOperand.append(rewrittenOperand.data(), rewrittenOperand.size());
  headerOperand.push_back(
      IncludeReplayProofContext::OrdinaryIncludeDelimiterClose(delimiterKind));
  replacement.replace(targetRange->begin, targetRange->end - targetRange->begin,
                      headerOperand);
  if (child.subkind == "#include_next" &&
      !rewriteIncludeNextDirectiveAsOrdinaryInclude(replacement))
    return std::nullopt;

  // Rewriting a clean child include operand is a parent-surface source edit,
  // not child expansion.  The proof witness is the child's own mapped token
  // envelope: the rewritten directive is accepted only because it is proven to
  // resolve to the same physical file and therefore reproduce the same child
  // PP-token range.  Certifying a normal include-preserving carrier keeps
  // strict theorem/audit mode from treating this deterministic closure repair
  // as an unproven emission artifact.
  IncludePatch patch{&child,          replacement,       child.cover.begin,
                     child.cover.end, child.cover.begin, child.cover.end};
  patch.hasDirectHeaderByteRange = true;
  patch.directHeaderByteBegin = siteStart;
  patch.directHeaderByteEnd = siteEnd;
  patch.directHeaderByteAuthority =
      DirectHeaderByteEditAuthorityKind::IncludeDirectiveRewrite;

  IncludeAnchorWitness witness;
  witness.evidence = IncludeAnchorEvidenceKind::MappedHeaderTokens;
  witness.hasByteRange = true;
  witness.startByte = siteStart;
  witness.endByte = siteEnd;
  witness.hasFirstPP = true;
  witness.firstPP = child.cover.begin;
  witness.hasLastPP = true;
  witness.lastPP = child.cover.end - 1;

  TextEdit edit{siteStart,    siteEnd, replacement, std::nullopt,
                std::nullopt, {},      {},          {}};
  const PreprocessingStructureKind includeKinds[] = {
      PreprocessingStructureKind::Include,
      PreprocessingStructureKind::IncludeNext,
      PreprocessingStructureKind::Import};
  if (!textEditAssembler_.AuthorizeProtectedSourceIntervals(
          edit, ProtectedSourceEditAuthorityKind::IncludeDirectiveRewrite,
          child.sitePath, child.parent, ownerBytes, siteStart, siteEnd,
          includeKinds, /*requireProtectedInterval=*/true,
          /*requestTerminalOnFailure=*/false))
    return std::nullopt;
  textEditAssembler_.AttachAcceptedResultCarrier(
      edit,
      proofLattice_.AcceptedCandidateBuilder().BuildAcceptedIncludeCandidate(
          AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens, patch,
          &witness));
  return edit;
}

const RefoldModel::HeaderDecl *
RefoldIncludeMaterializer::FindHeaderDeclForPatch(
    const RefoldModel::IncludeItem &inc, const IncludePatch &p) {
  return RefoldHeaderIncludeEditPlanner::FindHeaderDeclForPatch(inc, p);
}

RefoldIncludeMaterializer::IncludeTextEditPlan
RefoldIncludeMaterializer::ComputeIncludeTextEdits(
    const IncludeEdits &ie, std::string headerText) const {
  RefoldHeaderIncludeEditPlanner planner(
      model_, bSource_, aToks_, bToks_, bTokOff_, abTokMapA2B_, lineDirs_,
      sourceMapper_, paths_, macroStateProof_, lineControlProof_,
      ownerStateProof_, proofLattice_, textEditAssembler_,
      sidebandPragmaEdits_, lexLang_);
  return planner.Compute(ie, std::move(headerText));
}

std::optional<uint64_t>
RefoldIncludeMaterializer::ComputeChildBoundaryInsertByte(
    const IncludePatch &p, StringRef file,
    IncludeAnchorWitness *witness) const {
  RefoldHeaderIncludeEditPlanner planner(
      model_, bSource_, aToks_, bToks_, bTokOff_, abTokMapA2B_, lineDirs_,
      sourceMapper_, paths_, macroStateProof_, lineControlProof_,
      ownerStateProof_, proofLattice_, textEditAssembler_,
      sidebandPragmaEdits_, lexLang_);
  return planner.ComputeChildBoundaryInsertByte(p, file, witness);
}

} // namespace refold
} // namespace clang

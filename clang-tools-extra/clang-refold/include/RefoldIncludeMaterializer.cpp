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
#include "include/RefoldPragmaOnceGuardRewriter.h"

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

/// One macro patch already staged as a header TextEdit, with the A-token cover
/// its callsite realizes.
struct StagedMacroPatchEdit {
  /// Position of the staged edit in the header's edit list.
  size_t editIndex = 0;
  /// Producer macro invocation id realized by the staged edit.
  uint64_t macroId = 0;
  /// True when the model supplied a valid A-token cover for that invocation.
  bool hasCover = false;
  /// Inclusive first A token realized by the staged edit.
  uint64_t coverBegin = 0;
  /// Exclusive last A token realized by the staged edit.
  uint64_t coverEnd = 0;
};

/// Byte range and realized A-token cover declared by one mapped-header edit.
struct HeaderEnvelopeRealization {
  uint64_t startByte = 0;
  uint64_t endByte = 0;
  uint64_t coverBegin = 0;
  uint64_t coverEnd = 0;
};

/// Collect the mapped-header edits that state both the bytes they replace and
/// the A-token interval they realize.
///
/// Only the include-preserving mapped delete/replace path records that pair, on
/// its accepted carrier's anchor witness.  An edit without it declares no
/// realized cover, so nothing may be proved subsumed by it.
static SmallVector<HeaderEnvelopeRealization, 4> headerEnvelopeRealizations(
    ArrayRef<RefoldIncludeMaterializer::TextEdit> planEdits) {
  SmallVector<HeaderEnvelopeRealization, 4> realizations;
  for (const RefoldIncludeMaterializer::TextEdit &edit : planEdits) {
    for (const auto &carrier : edit.acceptedResults) {
      if (!carrier)
        continue;
      const ProofSummary &summary = carrier->proofSummary;
      if (summary.inventory.currentPath !=
          AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens)
        continue;
      if (!summary.hasIncludeAnchorWitness)
        continue;

      const IncludeAnchorWitness &witness = summary.includeAnchorWitness;
      if (!witness.hasByteRange || !witness.hasFirstPP || !witness.hasLastPP)
        continue;
      if (witness.startByte >= witness.endByte ||
          witness.firstPP > witness.lastPP)
        continue;

      realizations.push_back(
          HeaderEnvelopeRealization{witness.startByte, witness.endByte,
                                    witness.firstPP, witness.lastPP + 1});
    }
  }
  return realizations;
}

/// Discard staged macro-patch edits that a mapped-header edit already realizes,
/// or report that the two edit sets cannot be composed.
///
/// A mapped-header edit whose range was widened to a proven source envelope
/// replaces those bytes with B material covering the A tokens named by its
/// witness.  When a staged macro patch sits inside that byte range and realizes
/// a cover inside that same A interval, the envelope already emits it, so the
/// staged copy is a duplicate that would only collide at emission.  Every other
/// intersection -- a staged cover that escapes the envelope's, a partial byte
/// overlap, or a missing cover on either side -- leaves the two edits
/// genuinely incomparable, and the caller falls back rather than picking one.
static bool dropStagedMacroPatchEditsSubsumedByHeaderEnvelope(
    uint64_t includeId, ArrayRef<RefoldIncludeMaterializer::TextEdit> planEdits,
    ArrayRef<StagedMacroPatchEdit> stagedMacroPatchEdits,
    std::vector<RefoldIncludeMaterializer::TextEdit> &edits) {
  const SmallVector<HeaderEnvelopeRealization, 4> realizations =
      headerEnvelopeRealizations(planEdits);
  if (realizations.empty())
    return true;

  DenseSet<size_t> subsumed;
  for (const StagedMacroPatchEdit &staged : stagedMacroPatchEdits) {
    if (staged.editIndex >= edits.size())
      return false;
    const RefoldIncludeMaterializer::TextEdit &stagedEdit =
        edits[staged.editIndex];

    for (const HeaderEnvelopeRealization &realization : realizations) {
      if (stagedEdit.end <= realization.startByte ||
          realization.endByte <= stagedEdit.start)
        continue;

      const bool byteContained = realization.startByte <= stagedEdit.start &&
                                 stagedEdit.end <= realization.endByte;
      const bool coverContained = staged.hasCover &&
                                  realization.coverBegin <= staged.coverBegin &&
                                  staged.coverEnd <= realization.coverEnd;
      if (!byteContained || !coverContained) {
        REFOLD_LOG_TRACE("include/mat",
                         "inc#{0} staged macro patch macro#{1} bytes=[{2},{3}) "
                         "cover=[{4},{5}) is not realized by header envelope "
                         "bytes=[{6},{7}) cover=[{8},{9})",
                         includeId, staged.macroId, stagedEdit.start,
                         stagedEdit.end, staged.coverBegin, staged.coverEnd,
                         realization.startByte, realization.endByte,
                         realization.coverBegin, realization.coverEnd);
        return false;
      }
      subsumed.insert(staged.editIndex);
    }
  }

  if (subsumed.empty())
    return true;

  REFOLD_LOG_TRACE("include/mat",
                   "inc#{0} dropping {1} staged macro patch edit(s) already "
                   "realized by a widened header source envelope",
                   includeId, subsumed.size());

  std::vector<RefoldIncludeMaterializer::TextEdit> kept;
  kept.reserve(edits.size() - subsumed.size());
  for (size_t index = 0; index < edits.size(); ++index) {
    if (subsumed.contains(index))
      continue;
    kept.push_back(std::move(edits[index]));
  }
  edits = std::move(kept);
  return true;
}

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
    // Name the include.  Realizing it *from B* is what failed here, and the
    // ladder's response -- seeding it for ordinary materialization -- is a
    // different realization of the same include rather than a repeat of the one
    // that just failed, so naming it is a repair and not a loop.
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::IncludeRealizationBEnvelopeMapped,
            TerminalFallbackFailureReason::UnmappableIncludeBEnvelope,
            TerminalFallbackFailureContext::ForOwnerId(inc.id)),
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
    bool materializeIncludeNextInThisSubtree,
    std::optional<uint64_t> ancestorArmIdAtIncludeSite,
    const DenseSet<uint64_t> *ownersMustExpand) const {
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
        includeExpansionStartLineNos, includeExpansionAcceptedResults,
        ancestorArmIdAtIncludeSite);
    return;
  }

  // `#pragma once` is inert once this header's text lands in the TU, so its
  // state is re-expressed as macro state before any other header-local edit is
  // staged.  The rewriter owns the whole decision, including whether a guard is
  // needed at all.  A rejection means source-level once-state could not be
  // preserved, so fall back to realizing this include from B, which carries its
  // own guard proof and fails closed independently.
  if (PragmaOnceGuardEditResult guardResult =
          pragmaOnceGuards_.StageMaterializedBodyGuardEdits(
              *inc, headerPath, bytes, ancestorArmIdAtIncludeSite, edits);
      !guardResult.proven) {
    REFOLD_LOG_TRACE("pragma/once/guard",
                     "source materialization inc#{0} rejected: {1} ({2})",
                     includeId, toString(guardResult.rejection),
                     guardResult.detail);
    (void)TryRecordInlineIncludeRealizationFromB(
        *inc, guardResult.detail, includeExpansion,
        includeExpansionStartLineNos, includeExpansionAcceptedResults,
        ancestorArmIdAtIncludeSite);
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
  // Header source-envelope widening in pass 2 can extend one include patch over
  // bytes that a macro patch staged here already owns.  Remember which A-token
  // cover each staged edit realizes so that collision can be decided by proof
  // rather than by edit order.
  SmallVector<StagedMacroPatchEdit, 4> stagedMacroPatchEdits;

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
                  MacroStatePatchReplayInput{
                      mp.invRange.begin, mp.invRange.end,
                      StringRef(mp.replacement),
                      mp.materialized.replacementIsWhollyBPayload},
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

      StagedMacroPatchEdit staged;
      staged.editIndex = edits.size();
      staged.macroId = mp.macroId;
      for (const auto &inv : model_.GetMacroInvocations()) {
        if (inv.id != mp.macroId)
          continue;
        if (inv.cover.IsValid()) {
          staged.hasCover = true;
          staged.coverBegin = inv.cover.begin;
          staged.coverEnd = inv.cover.end;
        }
        break;
      }
      stagedMacroPatchEdits.push_back(staged);

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
        // Report why the include-preserving plan could not be discharged even
        // when the realization that follows succeeds.  Without this the reason
        // is observable only on the failure path, which hides the decision that
        // turns a header into a directive-free body.
        REFOLD_LOG_TRACE("include/mat",
                         "inc#{0} requires whole-include realization: {1}",
                         includeId, plan.realizationReason);
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

      // A header patch that widened to a full source envelope replaces its
      // whole byte range with B material, which necessarily deletes the
      // callsite of any macro patch pass 1 staged inside that range.  The two
      // edits then collide, and the applicator's composition law -- edits must
      // not overlap in original-byte space -- fails the whole translation unit
      // closed.
      //
      // Resolve the collision here, where both edit sets and their realized
      // A-token covers are in hand, instead of letting it reach emission.  The
      // widened envelope already realizes every A token in the cover it
      // declares, so a staged macro patch whose own cover lies inside that one
      // is emitted twice and the staged copy is redundant.  Anything else --
      // a cover that escapes the envelope, a partial byte overlap, or a
      // missing cover on either side -- is not proven redundant, so fall back
      // to whole-include realization rather than choosing between them.
      if (!dropStagedMacroPatchEditsSubsumedByHeaderEnvelope(
              includeId, plan.edits, stagedMacroPatchEdits, edits)) {
        AcceptedResultCandidate realizationCandidate;
        const std::string reason =
            llvm::formatv("staged macro patch in include #{0} is not provably "
                          "realized by a widened header source envelope",
                          includeId)
                .str();
        REFOLD_LOG_TRACE("include/mat",
                         "inc#{0} requires whole-include realization: {1}",
                         includeId, reason);
        if (auto realized = BuildInlineIncludeRealizationFromB(
                *inc, reason, &realizationCandidate)) {
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
      model_, perInclude, macroPatchesByOwner, children, sidebandPragmaEdits_,
      ownersMustExpand);

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
  // Site ranges handled by the child loop below.  A skipped include edge carries
  // no parent, so it never appears in `children`; the post-pass after this loop
  // finds those by physical site instead and must not re-handle a site the loop
  // already claimed.
  SmallVector<std::pair<uint64_t, uint64_t>, 8> claimedChildSites;

  // Physical headers whose bodies are inlined with a synthetic guard.  A child
  // whose closure reaches one of these cannot survive as a directive.
  const std::vector<std::string> guardedHeaderPaths =
      pragmaOnceGuards_.ActiveGuardedHeaderPaths();

  if (auto it = children.find(includeId); it != children.end()) {
    for (const auto *child : it->second) {
      {
        const uint64_t claimStart =
            std::clamp<uint64_t>(child->siteB, 0ULL, bytes.size());
        claimedChildSites.emplace_back(
            claimStart, extendIncludeDirectiveEnd(*child, bytes, claimStart));
      }
      bool todo = subtreeWork.HasDescendantWork(child->id);

      // Forced include-next materialization follows relocation, not
      // reachability.  `#include_next` resumes the search after the directory
      // of the file *containing* the directive, so it is perturbed exactly when
      // its own directive line is moved onto the flattened parent surface --
      // true for this edge only when the edge is itself an `#include_next`.  A
      // child preserved as a directive is a firewall: the compiler opens the
      // real file at its real location, so every directive inside it,
      // `#include_next` among them, is evaluated from the cursor the original
      // used.  Descendants that are themselves materialized re-test this at
      // their own level, through `forceIncludeNextMaterialization` below.
      //
      // Forcing on the whole subtree instead inlined headers whose text never
      // moves: a clean `<stdint.h>` with no descendant work was materialized
      // only because something below it spelled `#include_next`, could not be
      // materialized from source because its own arms use `__has_include_next`,
      // and so fell to a B realization that dropped every directive in its
      // subtree along with the include guards.
      if (materializeIncludeNextInThisSubtree &&
          child->subkind == "#include_next")
        todo = true;

      // A child that would survive as a directive but whose closure re-enters an
      // inlined once-header must be materialized instead.  The re-entry happens
      // inside an unmodified header, where no TU-space guard can reach it, so
      // preserving the directive would emit the guarded body a second time.
      // Materializing turns that nested directive into a surviving include
      // inside materialized text, which the ordinary wrapper does guard.
      if (!todo) {
        std::string reenteredPath;
        if (pragmaOnceGuards_.IncludeClosureReentersHeader(
                *child, guardedHeaderPaths, &reenteredPath)) {
          REFOLD_LOG_TRACE("pragma/once/guard",
                           "materializing child inc#{0} in inc#{1}: closure "
                           "re-enters inlined once-header '{2}'",
                           child->id, includeId, reenteredPath);
          todo = true;
        }
      }

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
          // This child survives as a real include directive inside the
          // materialized parent.  If it opens a header whose body was inlined
          // elsewhere, the directive must be guarded so it does not re-enter the
          // header that the inlined copy already emitted.
          const size_t ownerSize = bytes.size();
          const uint64_t childSiteStart =
              std::clamp<uint64_t>(child->siteB, 0ULL, ownerSize);
          const uint64_t childSiteEnd =
              extendIncludeDirectiveEnd(*child, bytes, childSiteStart);
          if (childSiteStart < childSiteEnd) {
            if (PragmaOnceGuardEditResult guardResult =
                    pragmaOnceGuards_.StageSurvivingIncludeGuardEdit(
                        *child, headerPath, includeId, bytes, childSiteStart,
                        childSiteEnd,
                        ComputeAncestorArmForChildInclude(
                            *child, includeId, ancestorArmIdAtIncludeSite),
                        edits);
                !guardResult.proven) {
              // Leaving an empty expansion here would silently delete the
              // *parent's* whole body while the caller consumed it as a
              // legitimate "materialized to nothing".  A child-guard rejection
              // is a proof failure for the emitted TU, so it must be terminal.
              REFOLD_LOG_TRACE(
                  "pragma/once/guard",
                  "nested surviving include inc#{0} in inc#{1} rejected: "
                  "{2} ({3})",
                  child->id, includeId, toString(guardResult.rejection),
                  guardResult.detail);
              // Name the include whose guard could not be proved.  Expanding
              // it is a repair -- it stops being a directive, so no surviving
              // occurrence is left to guard -- and the fallback ladder can only
              // try that for a region a request actually names.
              terminalSink_.RequestTerminalFallback(
                  MakeTerminalFallbackProofFailure(
                      TerminalFallbackObligationKind::
                          IncludeGuardStateStabilizable,
                      TerminalFallbackFailureReason::
                          IncludeGuardStateNotStabilizable,
                      TerminalFallbackFailureContext::ForOwnerId(child->id)),
                  "pragma/once/guard", guardResult.detail);
              includeExpansion[includeId] = std::string();
              return;
            }
          }

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
          appliedExpandedMacroRootIds, forceIncludeNextMaterialization,
          ComputeAncestorArmForChildInclude(*child, includeId,
                                            ancestorArmIdAtIncludeSite),
          ownersMustExpand);

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

  // 3b) Guard skipped include edges sited in this header.
  //
  // An include suppressed by `#pragma once` or by a header guard is never
  // entered, so the producer records no parent for it and it is invisible to the
  // child index above.  Those directives nevertheless survive verbatim in the
  // materialized text, and if their physical header was inlined at another
  // occurrence they would re-enter a body that is already in the output.  They
  // are therefore located by physical site rather than by include topology.
  {
    SmallVector<const RefoldModel::IncludeItem *, 4> skippedSiblings;
    for (const RefoldModel::IncludeItem &candidate : model_.GetIncludes()) {
      if (!paths_.PathsEqual(candidate.sitePath, headerPath))
        continue;
      const uint64_t siteStart =
          std::clamp<uint64_t>(candidate.siteB, 0ULL, bytes.size());
      const uint64_t siteEnd =
          extendIncludeDirectiveEnd(candidate, bytes, siteStart);
      if (siteStart >= siteEnd)
        continue;
      const bool claimed = llvm::any_of(
          claimedChildSites, [&](const std::pair<uint64_t, uint64_t> &claim) {
            return claim.first == siteStart && claim.second == siteEnd;
          });
      if (claimed)
        continue;
      if (!pragmaOnceGuards_.FindGuardForInclude(candidate))
        continue;
      skippedSiblings.push_back(&candidate);
    }

    // Repeated instances of one parent header produce several records for the
    // same physical site.  They describe identical bytes, so one wrapper per
    // distinct site is both necessary and sufficient.
    llvm::sort(skippedSiblings, [](const RefoldModel::IncludeItem *lhs,
                                   const RefoldModel::IncludeItem *rhs) {
      return std::tie(lhs->siteB, lhs->id) < std::tie(rhs->siteB, rhs->id);
    });
    std::optional<uint64_t> lastStagedSiteB;
    for (const RefoldModel::IncludeItem *sibling : skippedSiblings) {
      if (lastStagedSiteB && *lastStagedSiteB == sibling->siteB)
        continue;
      const uint64_t siteStart =
          std::clamp<uint64_t>(sibling->siteB, 0ULL, bytes.size());
      const uint64_t siteEnd =
          extendIncludeDirectiveEnd(*sibling, bytes, siteStart);
      if (PragmaOnceGuardEditResult guardResult =
              pragmaOnceGuards_.StageSurvivingIncludeGuardEdit(
                  *sibling, headerPath, includeId, bytes, siteStart, siteEnd,
                  ComputeAncestorArmForChildInclude(*sibling, includeId,
                                                    ancestorArmIdAtIncludeSite),
                  edits);
          !guardResult.proven) {
        // As above: an empty expansion would drop this header's whole body
        // rather than fail, so the rejection is propagated as terminal.
        REFOLD_LOG_TRACE("pragma/once/guard",
                         "skipped nested include inc#{0} in inc#{1} rejected: "
                         "{2} ({3})",
                         sibling->id, includeId,
                         toString(guardResult.rejection), guardResult.detail);
        // Same attribution as the nested-include rejection above: the
        // unguardable occurrence is the region at fault.
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::IncludeGuardStateStabilizable,
                TerminalFallbackFailureReason::
                    IncludeGuardStateNotStabilizable,
                TerminalFallbackFailureContext::ForOwnerId(sibling->id)),
            "pragma/once/guard", guardResult.detail);
        includeExpansion[includeId] = std::string();
        return;
      }
      lastStagedSiteB = sibling->siteB;
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

void RefoldIncludeMaterializer::CollectEnteredIncludeSubtree(
    uint64_t includeId, SmallVectorImpl<uint64_t> &subtree) const {
  // Only entered edges contribute content, and only an entered edge carries a
  // producer parent link, so walking parents is both exact and complete here.
  subtree.clear();
  DenseSet<uint64_t> visited;
  SmallVector<uint64_t, 16> worklist{includeId};
  while (!worklist.empty()) {
    const uint64_t current = worklist.pop_back_val();
    if (!visited.insert(current).second)
      continue;
    subtree.push_back(current);
    for (const RefoldModel::IncludeItem &candidate : model_.GetIncludes())
      if (candidate.parent && *candidate.parent == current)
        worklist.push_back(candidate.id);
  }
  llvm::sort(subtree);
}

std::optional<uint64_t>
RefoldIncludeMaterializer::ComputeAncestorArmForChildInclude(
    const RefoldModel::IncludeItem &child,
    std::optional<uint64_t> ownerIncludeId,
    std::optional<uint64_t> ancestorArmIdAtIncludeSite) const {
  // An ancestor arm already constrains everything below it: if the parent header
  // was itself reached from inside a conditional, nothing in its subtree is
  // unconditional, whatever the child's local nesting looks like.
  if (ancestorArmIdAtIncludeSite)
    return ancestorArmIdAtIncludeSite;

  // Conditional groups are scoped to a concrete include instance, so the owner
  // must match exactly: two instances of one header carry separate groups over
  // the same byte ranges.
  std::optional<uint64_t> innermost;
  uint64_t innermostBodySize = 0;
  for (const RefoldModel::CondGroup &group : model_.GetConds()) {
    if (!paths_.PathsEqual(group.file, child.sitePath))
      continue;
    // A translation-unit-owned group has no parent include, so nullopt on both
    // sides is the correct match rather than a missing owner.
    if (group.parentIncludeId != ownerIncludeId)
      continue;
    for (const RefoldModel::CondArm &arm : group.arms) {
      if (!arm.ContainsByte(child.siteB))
        continue;
      const uint64_t bodySize = arm.bodyE - arm.bodyB;
      if (!innermost || bodySize < innermostBodySize) {
        innermost = arm.id;
        innermostBodySize = bodySize;
      }
    }
  }
  return innermost;
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
  //
  // The producer records `condUsesHasInclude` from Clang's actual evaluation,
  // so it flags exactly the arms the preprocessor evaluated (the selected arm
  // and any earlier arms whose condition was tested and failed) and catches
  // macro-hidden or token-pasted operators a textual scan of `cond` would miss.
  // Arms that follow the selected one are never evaluated, so they are never
  // flagged -- which is sound, since they are unreachable under relocation when
  // no evaluated arm used the operator.
  for (const RefoldModel::CondGroup *group :
       model_.GetCondGroups(headerPath, includeId)) {
    if (!group)
      continue;
    for (const RefoldModel::CondArm &arm : group->arms) {
      if (arm.condUsesHasInclude)
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
        &includeExpansionAcceptedResults,
    std::optional<uint64_t> ancestorArmIdAtIncludeSite) const {
  AcceptedResultCandidate realizationCandidate;
  if (auto realized = BuildInlineIncludeRealizationFromB(
          include, reason, &realizationCandidate)) {
    // A body realized from B contains tokens, not header source, so it carries
    // no `#pragma once` to rewrite and would neither set nor consult the guard.
    // Emitting it unguarded beside a surviving include of the same header would
    // duplicate the body, so the guard is applied here as well.
    if (PragmaOnceGuardEditResult guardResult =
            pragmaOnceGuards_.StageRealizedFromBGuardText(
                include, ancestorArmIdAtIncludeSite, *realized);
        !guardResult.proven) {
      // Inline realization from B is the last realization path for this include,
      // so an unprovable guard here has no sound lower fallback.  Requesting the
      // terminal result is mandatory: leaving an empty expansion behind would be
      // consumed as a legitimate "materialized to nothing" and would silently
      // delete the header body while a surviving include of the same header
      // stayed unguarded.
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::IncludeGuardStateStabilizable,
              TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable,
              TerminalFallbackFailureContext::ForOwnerId(include.id)),
          "pragma/once/guard", guardResult.detail);
      REFOLD_LOG_TRACE("pragma/once/guard",
                       "B realization inc#{0} rejected: {1} ({2})", include.id,
                       toString(guardResult.rejection), guardResult.detail);
      includeExpansion[include.id] = std::string();
      return false;
    }

    // The token body absorbed the content of every header entered beneath this
    // include, and dropped all of their directives with it.  Restore each
    // header's own controlling macro so a later path back to that physical file
    // is skipped exactly as it was originally.
    SmallVector<uint64_t, 16> enteredSubtree;
    CollectEnteredIncludeSubtree(include.id, enteredSubtree);
    (void)pragmaOnceGuards_.AppendRealizedFromBIncludeGuardRestoration(
        enteredSubtree, *realized);

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

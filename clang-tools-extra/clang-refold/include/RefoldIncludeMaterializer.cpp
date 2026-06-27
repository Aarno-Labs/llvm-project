//===--- RefoldIncludeMaterializer.cpp -----------------*- C++ -*-===//
//
// Include materialization and include-owned edit planning.
//
// This file implements the service that realizes include subtrees, plans
// header-local edits, preserves include-owned sideband state, and assembles
// materialized include text from explicit proof and text-layout dependencies.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldLog.h"
#include "include/RefoldIncludeMaterializer.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/IncludeSpellingHelpers.h"
#include "include/RefoldIncludeReplayProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineControlEditHelpers.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "line-control/SourceLineDirectiveHelpers.h"
#include "macro/RefoldMacroStateProof.h"
#include "proof/NeutralSourceIslandProof.h"
#include "proof/RefoldSourceNeutralityProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTerminalProof.h"
#include "proof/SourceEnvelopeProof.h"
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
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ScopeExit.h"
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

  const auto wholeCover =
      sourceMapper_.MapATokRangeAToBTokenEnvelopeWholeCover(beginTok, endTok);
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
  auto consider =
      [&](const std::optional<std::pair<size_t, size_t>> &env) {
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

  consider(wholeCover);
  consider(preserveBoundary);
  consider(trimEdge);

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

  IncludePatch patch{&inc, std::move(insertBytes), h.aStart, h.aEnd, h.bStart,
                     h.bEnd};

  // Pre-materialization include patches are internal staging objects only. Do
  // not stamp a normalized accepted path here; the concrete preserving anchor
  // or realization class is chosen later during materialization, and only that
  // restamped result may cross a theorem-facing boundary.
  patch.proofSummary = proofLattice_.BuildIncludePatchProofSummary(
      /*realizedSurface=*/false, AcceptedPathKind::Unknown, &patch);

  return patch;
}

bool RefoldIncludeMaterializer::ValidateSidebandPragmaEditProof(
    const SidebandPragmaEdit &edit, StringRef stage) const {
  // Include materialization shares the same sideband proof failure classifier as
  // the TU path, but preserves the previous logging surface by suppressing the
  // engine-only success trace.
  return validateAndReportSidebandPragmaEditProof(
      edit, static_cast<uint64_t>(bSource_.size()), terminalSink_, stage,
      /*traceSuccess=*/false);
}

namespace {

/// Adapt staged header TextEdits into the narrow interval-only surface needed
/// by RefoldMacroStateProof.  Macro-state stabilization only has to reject
/// overlapping local edits; it must not depend on edit payloads, accepted
/// carriers, or text-assembly metadata.
static SmallVector<MacroStateStagedEditInterval, 8>
macroStateStagedEditIntervals(ArrayRef<RefoldIncludeMaterializer::TextEdit> edits) {
  SmallVector<MacroStateStagedEditInterval, 8> intervals;
  intervals.reserve(edits.size());
  for (const RefoldIncludeMaterializer::TextEdit &edit : edits)
    intervals.push_back(MacroStateStagedEditInterval{edit.start, edit.end});
  return intervals;
}

static bool isIncludeDirectiveHorizontalWhitespace(char c) {
  return c == '\r' || stringutils::isNonNewlineWs(c);
}

struct IncludeDirectiveHeaderOperandRange {
  size_t begin = 0;
  size_t end = 0;
};

// Locate pieces of a source-spelled include directive without rebuilding the
// directive from the normalized JSON spelling.  Comments are preprocessing
// whitespace for directive recognition, so they must be skipped while finding
// the syntactic header-name token, but preserved verbatim in the replacement
// text.
static bool consumeIncludeDirectiveEscapedNewline(StringRef text,
                                                  size_t &pos) {
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

} // namespace

std::optional<uint64_t>
RefoldIncludeMaterializer::ByteStartForPPInFile(StringRef file,
                                                uint64_t pp) const {
  auto it = model_.GetTokmapByPP().find(pp);
  if (it != model_.GetTokmapByPP().end() &&
      paths_.PathsEqual(it->second.file, file))
    return it->second.b;
  return std::nullopt;
}

std::optional<uint64_t>
RefoldIncludeMaterializer::ByteEndForPPInFile(StringRef file,
                                              uint64_t pp) const {
  auto it = model_.GetTokmapByPP().find(pp);
  if (it != model_.GetTokmapByPP().end() &&
      paths_.PathsEqual(it->second.file, file))
    return it->second.e;
  return std::nullopt;
}

RefoldIncludeMaterializer::TextEdit
RefoldIncludeMaterializer::MakeTextEditWithResyncOrPending(
    StringRef original, uint64_t start, uint64_t end, StringRef replacement,
    StringRef fileSpelling, std::optional<uint64_t> ownerIncludeId) const {
  ResyncOutcome outcome = textEditAssembler_.ApplyResyncOrPend(
      original, start, end, replacement, fileSpelling, ownerIncludeId);
  TextEdit edit{start, end, std::move(outcome.text),
                std::move(outcome.pending), std::nullopt, {}, {}};
  edit.lineControlPruneCandidates =
      std::move(outcome.lineControlPruneCandidates);
  return edit;
}

std::optional<std::string> RefoldIncludeMaterializer::BuildInlineIncludeRealizationFromB(
    const RefoldModel::IncludeItem &inc, StringRef reason,
    AcceptedResultCandidate *acceptedCandidate) const {
  // Inline include realization is theorem-facing only when the include's
  // A-cover can be projected to a concrete B-token envelope by an accepted
  // witness: either the canonical A-cover mapping or the boundary-stable
  // consensus proof.  If no such envelope exists, do not synthesize a weaker
  // realization; request the explicit terminal fallback instead.
  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvOpt = includeInsertionPlanner_
                        .ResolveIncludeRealizationBTokenEnvelope(
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
      proofLattice_.BuildAcceptedIncludeRealizationCandidate(
          AcceptedPathKind::IncludeRealizationInlineFromB, inc, evidenceKind,
          *bEnvOpt);
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

  auto pathNamesThisMaterializedHeader = [&](StringRef path) -> bool {
    if (path.empty())
      return false;
    if (path == StringRef(headerPath) || path == StringRef(headerLoadPath))
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
  };

  // Start from the header's original source bytes. Some callers preseed
  // `includeExpansion` with raw header text; otherwise load it deterministically
  // from the producer-selected physical file.  The logical headerPath above is
  // reserved for owner matching and `#line` spelling repair.
  std::string bytes;
  if (auto it = includeExpansion.find(includeId); it != includeExpansion.end())
    bytes = it->second;
  if (bytes.empty()) {
    auto bufOrErr =
        MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(headerLoadPath));
    if (!bufOrErr) {
      REFOLD_LOG_FATAL("include/mat",
                       "failed to read header: {0} (load path {1}) ({2})",
                       headerPath, headerLoadPath,
                       bufOrErr.getError().message());
    }

    const MemoryBuffer &mb = **bufOrErr;
    bytes.assign(mb.getBufferStart(), mb.getBufferEnd());
  }

  auto editsIt = perInclude.find(includeId);
  // Accumulate all local edits in this header byte space, then apply them once
  // at the end. This lets include edits, macro patches, and child include
  // materializations participate in one overlap/resync pass.
  std::vector<TextEdit> edits;

  auto materializedHeaderHasReplayContextSensitiveConditionalControl = [&]() {
    // A materialized header is replayed from the including surface, not from
    // the header's original file.  Conditional operators such as
    // __has_include perform quoted-header lookup relative to the file that
    // contains the directive; moving that directive out of its original header
    // can therefore flip the selected preprocessor branch even when ordinary
    // macro state is otherwise closed.  When such conditional control is part
    // of a materialized include, use the producer-proven B-token envelope for
    // the include realization instead of copying the directive text verbatim.
    for (const RefoldModel::CondGroup *group :
         model_.GetCondGroups(headerPath, includeId)) {
      if (!group)
        continue;
      for (const RefoldModel::CondArm &arm : group->arms) {
        if (!arm.cond)
          continue;
        if (arm.cond->contains("__has_include"))
          return true;
      }
    }
    return false;
  };

  auto materializedHeaderHasIncludeLevelObserver = [&]() {
    // __INCLUDE_LEVEL__ observes include-stack depth, not line/file state.
    // Once an include owner is materialized, the source-spelled header bytes are
    // replayed from the flattened parent/TU surface instead of being entered by
    // an actual #include edge.  Leaving a preserved __INCLUDE_LEVEL__ spelling
    // in that materialized text would therefore expand at the replay depth
    // rather than at the producer-observed depth.  Treat any observable
    // __INCLUDE_LEVEL__ in this owner as a request for producer-proven B-surface
    // realization, which literalizes the recorded expansion value and avoids
    // fabricating an include-stack transition that no longer exists.
    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations()) {
      if (macro.name != "__INCLUDE_LEVEL__")
        continue;

      const RefoldModel::MacroInvocation *site =
          lineControlProof_.LineStateObservableMacroSite(macro);
      std::optional<uint64_t> owner =
          site && site->ownerIncludeId ? site->ownerIncludeId
                                       : macro.ownerIncludeId;
      if (!owner || *owner != includeId)
        continue;

      // The owner id is the semantic proof; the optional invocation file is a
      // hardening check against malformed producer records that accidentally
      // attach a builtin from another source buffer to this include instance.
      if (site && site->invFile &&
          !pathNamesThisMaterializedHeader(*site->invFile))
        continue;
      if (macro.invFile && !pathNamesThisMaterializedHeader(*macro.invFile) &&
          (!site || !site->invFile ||
           !pathNamesThisMaterializedHeader(*site->invFile)))
        continue;

      return true;
    }
    return false;
  };

  auto realizeMaterializedHeaderFromB = [&](StringRef reason) -> bool {
    AcceptedResultCandidate realizationCandidate;
    if (auto realized =
            BuildInlineIncludeRealizationFromB(*inc, reason,
                                               &realizationCandidate)) {
      includeExpansion[includeId] = std::move(*realized);
      includeExpansionStartLineNos[includeId] = 1;
      includeExpansionAcceptedResults[includeId] =
          std::move(realizationCandidate);
      REFOLD_LOG_TRACE("include/mat", "realize include #{0} from B: {1}",
                       includeId, reason);
      return true;
    }

    includeExpansion[includeId] = std::string();
    return false;
  };

  if (materializedHeaderHasReplayContextSensitiveConditionalControl()) {
    (void)realizeMaterializedHeaderFromB(
        "replay-context-sensitive conditional control in materialized header");
    return;
  }

  if (materializedHeaderHasIncludeLevelObserver()) {
    (void)realizeMaterializedHeaderFromB(
        "__INCLUDE_LEVEL__ observer in materialized header");
    return;
  }

  auto materializedHeaderHasNoLineRepairableFileObserver = [&]() {
    // `__FILE__` and `__FILE_NAME__` observe the current logical filename.  If
    // line directives are disabled, a materialized header body is replayed as
    // ordinary text in the flattened parent/output source, so preserving a naked
    // builtin would make it observe the emitted `.c.mod` context rather than the
    // producer-selected header.  The only deterministic no-lines repair is the
    // already-preprocessed B realization, which literalizes the producer value.
    if (lineDirs_.Enabled())
      return false;

    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations()) {
      if (macro.name != "__FILE__" && macro.name != "__FILE_NAME__")
        continue;
      if (!lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(macro))
        continue;

      const RefoldModel::MacroInvocation *site =
          lineControlProof_.LineStateObservableMacroSite(macro);
      std::optional<uint64_t> owner =
          site && site->ownerIncludeId ? site->ownerIncludeId
                                       : macro.ownerIncludeId;
      if (!owner || *owner != includeId)
        continue;

      // The owner id is the semantic proof.  The file checks only reject
      // malformed records that accidentally attach a preserved builtin from some
      // other source buffer to this include instance.
      if (site && site->invFile &&
          !pathNamesThisMaterializedHeader(*site->invFile))
        continue;
      if (macro.invFile && !pathNamesThisMaterializedHeader(*macro.invFile) &&
          (!site || !site->invFile ||
           !pathNamesThisMaterializedHeader(*site->invFile)))
        continue;

      return true;
    }
    return false;
  };

  auto materializedHeaderHasUnrepairableFileSpellingObserver = [&]() {
    // With line directives enabled, materialization can preserve source-spelled
    // `__FILE__` / `__FILE_NAME__` only if the include edge has a producer
    // filename spelling to install before the copied body.  If the map lacks any
    // entered-file witness, do not invent one from the target token; literalize
    // the already-preprocessed value instead.
    if (!lineDirs_.Enabled())
      return false;

    const LineStateObserverDemand demand =
        lineControlProof_.IncludeSubtreeLineStateObserverDemand(includeId);
    if (!demand.needsFile && !demand.needsFileName)
      return false;

    StringRef producerSpelling = producerEnteredFileSpelling(*inc);
    if (producerSpelling.empty())
      return true;

    if (demand.needsFileName && producerEnteredFileName(*inc).empty())
      return true;

    return false;
  };

  if (materializedHeaderHasNoLineRepairableFileObserver()) {
    (void)realizeMaterializedHeaderFromB(
        "__FILE__/__FILE_NAME__ observer in materialized header without "
        "line repair");
    return;
  }

  if (materializedHeaderHasUnrepairableFileSpellingObserver()) {
    (void)realizeMaterializedHeaderFromB(
        "__FILE__/__FILE_NAME__ observer in materialized header without "
        "producer entered-file spelling");
    return;
  }

  auto materializedHeaderHasNoLineRepairableBaseFileObserver = [&]() {
    // __BASE_FILE__ is part of the line/file observer family when line
    // directives are available: a source-authored or synthetic #line can keep
    // the replayed spelling stable without destroying source structure.  In
    // --no-lines mode, however, a materialized include owner has no way to
    // repair a preserved source-spelled __BASE_FILE__; replay would observe the
    // refolded output as the top-level file.  In that mode, realize the include
    // from the producer-proven B envelope so the recorded literal is emitted.
    if (lineDirs_.Enabled())
      return false;

    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations()) {
      if (macro.name != "__BASE_FILE__")
        continue;
      if (!lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(macro))
        continue;

      const RefoldModel::MacroInvocation *site =
          lineControlProof_.LineStateObservableMacroSite(macro);
      std::optional<uint64_t> owner =
          site && site->ownerIncludeId ? site->ownerIncludeId
                                       : macro.ownerIncludeId;
      if (!owner || *owner != includeId)
        continue;

      // The owner id is the semantic proof.  The file spelling checks are only
      // hardening against malformed producer records that accidentally attach a
      // builtin from another source buffer to this include instance.
      if (site && site->invFile &&
          !pathNamesThisMaterializedHeader(*site->invFile))
        continue;
      if (macro.invFile && !pathNamesThisMaterializedHeader(*macro.invFile) &&
          (!site || !site->invFile ||
           !pathNamesThisMaterializedHeader(*site->invFile)))
        continue;

      return true;
    }
    return false;
  };

  if (materializedHeaderHasNoLineRepairableBaseFileObserver()) {
    (void)realizeMaterializedHeaderFromB(
        "__BASE_FILE__ observer in materialized header without line repair");
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

      StringRef patchEnvelope =
          refoldSliceTokenEnvelope(bTokOff_, bSource_, patch.bStart, patch.bEnd);
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
    if (!ValidateSidebandPragmaEditProof(sideband, "pragma/sideband/include")) {
      includeExpansion[includeId] = std::string();
      return;
    }

    if (!sideband.SourceIsWithinOwnerBytes(static_cast<uint64_t>(bytes.size()))) {
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
        lineControlProof_.OwnerSuffixHasLineStateSensitiveBuiltin(includeId, headerPath,
                                                sourceRange.second);
    ResyncOutcome ro =
        mustPreserveHeaderLineState
            ? textEditAssembler_.ApplyResyncOrPend(bytes, sourceRange.first, sourceRange.second,
                                sideband.ReplacementText(), headerPath,
                                includeId)
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
    textEditAssembler_.AttachAcceptedResultCarrier(
        edit, proofLattice_.BuildAcceptedIncludeRealizationCandidate(
                  AcceptedPathKind::IncludeMaterializedExpansion, *inc));
    edits.push_back(std::move(edit));
  }

  // Replay-context stability for include-owned macro patches is planned by
  // RefoldMacroStateProof.cpp.  MaterializeIncludeExpansion() keeps ownership
  // of TextEdit construction, local line-resync, accepted-result attachment,
  // and include-recursion control flow.

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
          StringRef(bytes), mp.invEnd, mp.replacement);
      uint64_t editStart = mp.invStart;
      uint64_t editEnd = mpEnd;
      std::string editReplacement = mp.replacement;
      if (std::optional<StabilizedMaterializedHeaderMacroPatch> stabilized =
              macroStateProof_.StabilizeMaterializedHeaderMacroPatchReplay(
                  MacroStatePatchReplayInput{mp.invStart, mp.invEnd,
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
      }

      // Apply local line-resync policy before staging the edit, and carry the
      // accepted macro proof metadata to the emitted TextEdit.
      ResyncOutcome ro = textEditAssembler_.ApplyResyncOrPend(
          bytes, editStart, editEnd, editReplacement, headerPath, includeId);
      TextEdit edit{editStart,
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
      textEditAssembler_.AttachAcceptedResultCarrier(edit, proofLattice_.BuildAcceptedEmittedMacroCandidate(mp));
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

  // Return true if this include subtree contains any work that requires
  // materialization: local include edits, local macro patches, or descendant
  // includes with work. Clean child subtrees are left as spelled directives.
  auto includeHasSidebandPragmaWork = [&](uint64_t id) {
    return llvm::any_of(sidebandPragmaEdits_, [&](const SidebandPragmaEdit &e) {
      return e.TargetsInclude(id);
    });
  };

  auto includeHasLineDirectiveForcingSidebandWork = [&](uint64_t id) {
    return llvm::any_of(sidebandPragmaEdits_, [&](const SidebandPragmaEdit &e) {
      return e.TargetsInclude(id) && e.ForcesIncludeLineDirectiveWrappers();
    });
  };

  auto includeHasOrdinaryReplayTokens = [&](uint64_t id) {
    if (const auto *item = model_.GetIncludeById(id))
      return item->cover.end > item->cover.begin;
    return false;
  };

  // Return true when the materialized include subtree needs replay provenance
  // only from sideband pragma edits.  This is intentionally separate from the
  // #line-wrapper classification below: visible sideband replacement text may
  // force ordinary enter/exit #line wrappers while the include still has a
  // zero-token normal PP cover, so its edit-map witness must come from the
  // sideband B-byte envelope rather than from a nonexistent token envelope.
  auto includeUsesOnlySidebandReplayEnvelope =
      [&](auto &&self, uint64_t id) -> bool {
    bool sawSideband = includeHasSidebandPragmaWork(id);
    if (includeHasOrdinaryReplayTokens(id))
      return false;
    if (auto it = perInclude.find(id);
        it != perInclude.end() && !it->second.patches.empty())
      return false;
    if (auto it = macroPatchesByOwner.find(std::optional<uint64_t>(id));
        it != macroPatchesByOwner.end() && !it->second.empty())
      return false;
    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        if (!self(self, child->id))
          return false;
        sawSideband = true;
      }
    }
    return sawSideband;
  };

  // A sideband-only include materialization is the one class where omitting
  // otherwise-unobservable #line wrappers is intentional.  The invariant is not
  // "materialized because of a sideband edit"; it is stricter: the materialized
  // owner must have no ordinary PP-token replay of its own.  Once we emit
  // normal header tokens while replacing an include directive, --with-lines
  // must preserve the historical enter/exit file transition even if no current
  // token observes that state.  Ordinary include patches, macro patches, B-only
  // header sideband insertions, and non-empty include token covers all re-enter
  // that policy.
  enum class MaterializationWorkClass { None, SidebandPragmaOnly, Ordinary };
  auto classifyMaterializationWork =
      [&](auto &&self, uint64_t id) -> MaterializationWorkClass {
    bool sawSideband = includeHasSidebandPragmaWork(id);
    if (includeHasLineDirectiveForcingSidebandWork(id))
      return MaterializationWorkClass::Ordinary;
    if (sawSideband && includeHasOrdinaryReplayTokens(id))
      return MaterializationWorkClass::Ordinary;
    if (auto it = perInclude.find(id);
        it != perInclude.end() && !it->second.patches.empty())
      return MaterializationWorkClass::Ordinary;
    if (auto it = macroPatchesByOwner.find(std::optional<uint64_t>(id));
        it != macroPatchesByOwner.end() && !it->second.empty())
      return MaterializationWorkClass::Ordinary;
    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        switch (self(self, child->id)) {
        case MaterializationWorkClass::Ordinary:
          return MaterializationWorkClass::Ordinary;
        case MaterializationWorkClass::SidebandPragmaOnly:
          sawSideband = true;
          break;
        case MaterializationWorkClass::None:
          break;
        }
      }
    }
    return sawSideband ? MaterializationWorkClass::SidebandPragmaOnly
                       : MaterializationWorkClass::None;
  };

  // Include replay proof is read-only.  Keep the function_ref service
  // callables as named locals so their storage outlives the proof context that
  // references them; the context itself remains scoped to this materialization
  // operation, preserving the per-expansion search-dir cache lifetime.
  auto includeReplaySliceASource = [this](uint64_t begin,
                                          uint64_t end) -> StringRef {
    return sourceMapper_.SliceASource(begin, end);
  };
  auto includeReplaySamePhysicalIncludeFile =
      [this](StringRef candidatePath,
             const RefoldModel::IncludeItem &include) -> bool {
    return paths_.SamePhysicalIncludeFile(candidatePath, include);
  };
  auto includeReplayLineStateObservableMacroSite =
      [this](const RefoldModel::MacroInvocation &macro)
      -> const RefoldModel::MacroInvocation * {
    return lineControlProof_.LineStateObservableMacroSite(macro);
  };
  auto includeReplayLineStateBuiltinInvocationIsPreservedObserver =
      [this](const RefoldModel::MacroInvocation &macro) -> bool {
    return lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(macro);
  };
  auto includeReplayIncludeSubtreeLineStateObserverDemand =
      [this](uint64_t includeId) -> LineStateObserverDemand {
    return lineControlProof_.IncludeSubtreeLineStateObserverDemand(includeId);
  };

  IncludeReplayProofInputs includeReplayProofInputs{
      model_, aSource_, lineDirs_, finalReplaySurface_};
  IncludeReplayProofServices includeReplayProofServices{
      includeReplaySliceASource,
      includeReplaySamePhysicalIncludeFile,
      includeReplayLineStateObservableMacroSite,
      includeReplayLineStateBuiltinInvocationIsPreservedObserver,
      includeReplayIncludeSubtreeLineStateObserverDemand};
  IncludeReplayProofContext includeReplayProof(includeReplayProofInputs,
                                               includeReplayProofServices);

  auto extendIncludeDirectiveEnd = [&](const RefoldModel::IncludeItem &item,
                                       StringRef ownerBytes,
                                       uint64_t siteStart) -> uint64_t {
    uint64_t siteEnd = std::clamp<uint64_t>(item.siteE, siteStart,
                                            ownerBytes.size());
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
  };

  auto rewriteIncludeNextDirectiveAsOrdinaryInclude =
      [](std::string &replacement) -> bool {
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
  };

  auto makeCleanChildIncludeOperandRewriteEdit =
      [&](const RefoldModel::IncludeItem &child, const std::string &operand,
          IncludeReplayProofContext::OrdinaryIncludeDelimiterKind delimiterKind,
          StringRef ownerBytes) -> std::optional<TextEdit> {
    const size_t n = ownerBytes.size();
    const uint64_t siteStart = std::clamp<uint64_t>(child.siteB, 0ULL, n);
    const uint64_t siteEnd = extendIncludeDirectiveEnd(child, ownerBytes,
                                                       siteStart);
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
    headerOperand += operand;
    headerOperand.push_back(
        IncludeReplayProofContext::OrdinaryIncludeDelimiterClose(delimiterKind));
    replacement.replace(targetRange->begin,
                        targetRange->end - targetRange->begin, headerOperand);
    if (child.subkind == "#include_next" &&
        !rewriteIncludeNextDirectiveAsOrdinaryInclude(replacement))
      return std::nullopt;

    // Rewriting a clean child include operand is a parent-surface source edit,
    // not child expansion.  The proof witness is the child's own mapped token
    // envelope: the rewritten directive is accepted only because it is proven to
    // resolve to the same physical file and therefore to reproduce the same
    // child PP-token range.  Stamping a normal include-preserving carrier keeps
    // strict theorem/audit mode from treating this deterministic closure repair
    // as an unproven emission artifact.
    IncludePatch patch{&child,
                       replacement,
                       child.cover.begin,
                       child.cover.end,
                       child.cover.begin,
                       child.cover.end};
    patch.hasDirectHeaderByteRange = true;
    patch.directHeaderByteBegin = siteStart;
    patch.directHeaderByteEnd = siteEnd;

    IncludeAnchorWitness witness;
    witness.evidence = IncludeAnchorEvidenceKind::MappedHeaderTokens;
    witness.hasByteRange = true;
    witness.startByte = siteStart;
    witness.endByte = siteEnd;
    witness.hasFirstPP = true;
    witness.firstPP = child.cover.begin;
    witness.hasLastPP = true;
    witness.lastPP = child.cover.end - 1;

    TextEdit edit{siteStart, siteEnd, replacement, std::nullopt, std::nullopt,
                  {}, {}, {}};
    textEditAssembler_.AttachAcceptedResultCarrier(
        edit, proofLattice_.BuildAcceptedIncludeCandidate(
                  AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens,
                  patch, &witness));
    return edit;
  };

  // This predicate answers only whether a descendant already carries real
  // materialization work: token/sideband include patches or macro patches.
  // It must not recursively apply the clean-child replay-context test.  That
  // test is meaningful only for the immediate child directive that would be
  // copied into the currently materialized surface.  If a clean child remains
  // an include directive, its own children are still replayed from the child's
  // original header context, not from the outer materialized surface.
  auto hasDescendantWork = [&](auto &&self, uint64_t id) -> bool {
    bool selfWork = includeHasSidebandPragmaWork(id);
    if (auto it = perInclude.find(id); it != perInclude.end())
      selfWork = selfWork || !it->second.patches.empty();
    if (!selfWork) {
      if (auto it = macroPatchesByOwner.find(id);
          it != macroPatchesByOwner.end())
        selfWork = !it->second.empty();
    }

    if (selfWork) {
      return true;
    }

    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        if (self(self, child->id)) {
          return true;
        }
      }
    }

    return false;
  };

  // When an ancestor clean-child replay proof failed because of descendant
  // #include_next state, recursive materialization must walk all the way down
  // to every affected include-next edge.  A direct-child-only test is not
  // sufficient: a clean ordinary include can contain a nested #include_next and
  // otherwise have no edits.  If we allowed that ordinary include to remain as
  // a directive while this force flag is active, the final output could still
  // replay an include-next cursor from a relocated context.
  //
  // This helper is intentionally only a reachability predicate.  The separate
  // include-next replay proof decides when a clean include can be preserved
  // normally.
  // Once materializeIncludeNextInThisSubtree is set, however, the caller has
  // already failed to prove at least one include-next obligation in this
  // subtree, so the safe repair is to materialize through the paths that can
  // reach include-next directives.
  auto subtreeContainsIncludeNext = [&](auto &&self, uint64_t id) -> bool {
    const RefoldModel::IncludeItem *include = model_.GetIncludeById(id);
    if (include && include->subkind == "#include_next")
      return true;

    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second)
        if (child && self(self, child->id))
          return true;
    }

    return false;
  };

  // 3) Materialize child includes only when their subtree has work. For each
  // dirty child, recursively build the child's fully materialized text and then
  // replace the child's include directive in this header.
  if (auto it = children.find(includeId); it != children.end()) {
    for (const auto *child : it->second) {
      bool todo = hasDescendantWork(hasDescendantWork, child->id);

      // Forced include-next materialization is subtree-wide, not
      // direct-child-only.  The bug class this machinery protects against is a
      // materialized header that preserves a clean ordinary child include whose
      // nested #include_next then resumes lookup from the wrong HeaderSearch
      // cursor.  Force materialization along any path that can reach
      // #include_next so the directive itself is eventually replaced by the
      // producer-selected subtree unless a normal, local proof has already kept
      // us out of this forced mode.
      if (materializeIncludeNextInThisSubtree &&
          subtreeContainsIncludeNext(subtreeContainsIncludeNext, child->id))
        todo = true;

      IncludeReplayProofContext::CleanChildIncludeReplayPlan replayPlan;
      if (!todo) {
        replayPlan =
            includeReplayProof.PlanCleanChildIncludeReplayFromMaterializedParent(
                *child);
        if (replayPlan.action ==
            IncludeReplayProofContext::CleanChildIncludeReplayAction::
                RewriteOperand) {
          if (std::optional<TextEdit> rewrite =
                  makeCleanChildIncludeOperandRewriteEdit(
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
            replayPlan.action =
                IncludeReplayProofContext::CleanChildIncludeReplayAction::
                    Materialize;
            if (subtreeContainsIncludeNext(subtreeContainsIncludeNext,
                                           child->id))
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
      MaterializeIncludeExpansion(child->id, perInclude, macroPatchesByOwner,
                                  children, includeExpansion,
                                  includeExpansionLineControlPruneCandidates,
                                  includeExpansionLineControlSourceMappings,
                                  includeExpansionStartLineNos,
                                  includeExpansionAcceptedResults,
                                  appliedExpandedMacroRootIds,
                                  forceIncludeNextMaterialization);

      const auto &childText = includeExpansion[child->id];
      const size_t n = bytes.size();
      const uint64_t siteStart = std::clamp<uint64_t>(child->siteB, 0ULL, n);
      uint64_t siteEnd = extendIncludeDirectiveEnd(*child, bytes, siteStart);

      if (siteStart < siteEnd) {
        const bool sidebandOnly = classifyMaterializationWork(
                                      classifyMaterializationWork, child->id) ==
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
        LineControlWrappedText wrapped = lineObserverLayout_.WrapIncludeExpansionForMaterialization(
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
            includeUsesOnlySidebandReplayEnvelope(
                includeUsesOnlySidebandReplayEnvelope, child->id)) {
          if (auto sidebandBRange =
                  textEditAssembler_.SidebandPragmaMaterializedBByteRangeForInclude(child->id))
            textEditAssembler_.StampTextEditMaterializedBByteRange(edit, sidebandBRange->first,
                                                sidebandBRange->second);
        }

        // Preserve the child's accepted-result carrier if the child was
        // materialized through a specific realization path; otherwise record
        // the generic materialized-expansion carrier for this child include.
        auto itAccepted = includeExpansionAcceptedResults.find(child->id);
        if (itAccepted != includeExpansionAcceptedResults.end()) {
          textEditAssembler_.AttachAcceptedResultCarrier(edit, itAccepted->second);
        } else {
          textEditAssembler_.AttachAcceptedResultCarrier(
              edit,
              proofLattice_.BuildAcceptedIncludeRealizationCandidate(
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

  auto computeFirstEmittedHeaderLine = [&]() -> size_t {
    if (edits.empty())
      return 1;

    SmallVector<const TextEdit *, 8> ordered;
    ordered.reserve(edits.size());
    for (const TextEdit &edit : edits)
      ordered.push_back(&edit);
    llvm::sort(ordered, [](const TextEdit *lhs, const TextEdit *rhs) {
      if (lhs->start != rhs->start)
        return lhs->start < rhs->start;
      return lhs->end < rhs->end;
    });

    // Track the first physical line that will actually be emitted from the
    // materialized header.  A leading sideband deletion consumes the directive
    // line before wrapping, so the include-enter #line must name the first
    // surviving source line, not blindly line 1 of the header.  A leading
    // replacement still occupies the replaced directive's logical line.
    uint64_t cursor = 0;
    for (const TextEdit *edit : ordered) {
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
  };

  const size_t firstEmittedHeaderLine = computeFirstEmittedHeaderLine();

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
      proofLattice_.BuildAcceptedIncludeRealizationCandidate(
          AcceptedPathKind::IncludeMaterializedExpansion, *inc);
}

const RefoldModel::HeaderDecl *
RefoldIncludeMaterializer::FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                                     const IncludePatch &p) {
  if (inc.decls.empty())
    return nullptr;

  const uint64_t aLo = p.aStart;
  const uint64_t aHi = p.aEnd;

  const RefoldModel::HeaderDecl *bestCover = nullptr;
  const RefoldModel::HeaderDecl *bestOverlap = nullptr;

  auto getSpanLen = [](const RefoldModel::HeaderDecl *d) -> uint64_t {
    // Treat malformed declaration spans as zero-length for tie-breaking rather
    // than allowing unsigned underflow.
    if (d->span.end <= d->span.begin)
      return 0;
    return d->span.end - d->span.begin;
  };

  for (const auto &d : inc.decls) {
    const uint64_t dLo = d.span.begin;
    const uint64_t dHi = d.span.end;
    const uint64_t curSpanLen = dHi - dLo;

    // For a pure insertion, attach the patch to the smallest declaration whose
    // closed declaration range contains the insertion point. The end boundary
    // is intentionally inclusive so insertions immediately after a declaration
    // body still belong to that declaration.
    if (aLo == aHi) {
      if (aLo >= dLo && aLo <= dHi) {
        if (!bestCover || curSpanLen < getSpanLen(bestCover))
          bestCover = &d;
      }
      continue;
    }

    // For non-empty patches, prefer a declaration that fully contains the
    // edited A interval. If no declaration covers the interval, fall back to
    // the smallest declaration that merely overlaps it.
    const bool covers = (aLo >= dLo && aHi <= dHi);
    const bool overlaps = (aLo < dHi && aHi > dLo);

    if (covers) {
      if (!bestCover || curSpanLen < getSpanLen(bestCover))
        bestCover = &d;
    } else if (overlaps) {
      if (!bestOverlap || curSpanLen < getSpanLen(bestOverlap))
        bestOverlap = &d;
    }
  }

  return bestCover ? bestCover : bestOverlap;
}

RefoldIncludeMaterializer::IncludeTextEditPlan
RefoldIncludeMaterializer::ComputeIncludeTextEdits(const IncludeEdits &ie,
                                      std::string headerText) const {
  const std::string file = refoldIncludeEnteredFileSpelling(*ie.include);

  const size_t fileLen = headerText.size();

  // PP cover for this include inside the header; if not present, these will
  // already have been derived from spans when building the model.
  const uint64_t coverBegin =
      std::max(static_cast<uint64_t>(0), ie.include->cover.begin);
  const uint64_t coverEnd = std::max(coverBegin, ie.include->cover.end);

  // Single list of edits; we will apply them highest-offset-first so indices
  // remain stable as we mutate the StringBuilder.
  IncludeTextEditPlan plan;

  using HeaderMacroStateDirectivePiece = MacroStateDirectiveLineInterval;

  struct HeaderPreservedGapPiece {
    enum class Kind {
      MacroStateDirective,
      ZeroTokenConditionalGroup,
      ZeroTokenMacroInvocation,
      ZeroTokenChildInclude,
      BalancedPragmaStateIsland
    };

    Kind kind = Kind::MacroStateDirective;
    const RefoldModel::MacroDirective *directive = nullptr;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t id = 0;
  };

  // Recover the complete physical source line for a macro-state directive that
  // was spelled in this include expansion. The shared helper owns the
  // macro-name-anchor reconstruction and exact-byte validation.
  auto headerMacroStateDirectiveInterval =
      [&](const RefoldModel::MacroDirective &directive) {
        return macroStateProof_.RecoverMacroStateDirectiveLineInterval(
            directive, file, StringRef(headerText),
            std::optional<uint64_t>(ie.include->id));
      };

  const HeaderSourceNeutralityContext headerSourceNeutrality =
      RefoldSourceNeutralityProof::BuildHeaderSourceNeutralityContext(
          model_, macroStateProof_, paths_, StringRef(headerText), file,
          ie.include->id, isWsOrCompleteCommentTrivia);

  std::function<bool(const RefoldModel::MacroInvocation &)>
      headerMacroInvocationIsSourceNeutralZeroToken;
  std::function<bool(const RefoldModel::IncludeItem &)>
      headerIncludeIsSourceNeutralZeroToken;

  // Return true iff a recorded conditional group in this materialized header
  // can be preserved as a complete zero-token gap piece. This is the
  // header-owned counterpart of the TU zero-token conditional-gap proof: the
  // group bytes may be carried forward only when the producer recorded no
  // selected-arm PP material and every selected-arm body artifact either has no
  // source-state effect or discharges its own neutral proof. Macro invocations
  // in conditional-control lines are allowed because the complete group is
  // preserved verbatim; arm-body macro invocations must prove zero-token
  // neutrality through the ordinary header macro-gap proof.
  auto headerConditionalGroupIsPreservableGap =
      [&](const RefoldModel::CondGroup &group, uint64_t gapBegin,
          uint64_t gapEnd) -> bool {
    auto includeIsNeutral = [&](const RefoldModel::IncludeItem &inc) {
      return headerIncludeIsSourceNeutralZeroToken &&
             headerIncludeIsSourceNeutralZeroToken(inc);
    };
    NeutralConditionalIslandContext islandContext{
        /*requireGroupBeginAtLineStart=*/true,
        NeutralConditionalArmSpanMode::SelectedArmsOnly};
    return RefoldSourceNeutralityProof::ConditionalGroupIsNeutralIsland(
        headerSourceNeutrality, group, gapBegin, gapEnd, islandContext,
        includeIsNeutral);
  };

  auto preservedGapPieceText = [&](const HeaderPreservedGapPiece &piece) {
    // Macro-state directives are preserved from the producer's recorded text so
    // spelling such as leading whitespace and line continuations matches the
    // directive record, not a best-effort slice reconstructed from ownership.
    if (piece.kind == HeaderPreservedGapPiece::Kind::MacroStateDirective &&
        piece.directive)
      return piece.directive->text.str();

    // Other preserved gap pieces are source intervals in the materialized
    // header buffer.  Require a valid in-buffer range before copying bytes into
    // the replacement payload.
    if (piece.end <= headerText.size() && piece.begin <= piece.end)
      return StringRef(headerText).slice(piece.begin, piece.end).str();
    return std::string();
  };

  // Recursively prove header-spelled zero-token macro invocations.  The shared
  // source-neutrality adapter preserves the materializer's historical
  // whole-definition-list tiling shortcut while centralizing the recursive
  // replacement-list proof scaffold.
  headerMacroInvocationIsSourceNeutralZeroToken =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    return RefoldSourceNeutralityProof::MacroInvocationIsSourceNeutralZeroToken(
        headerSourceNeutrality, m);
  };

  auto headerMacroInvocationIsPreservableGap =
      [&](const RefoldModel::MacroInvocation &m, uint64_t gapBegin,
          uint64_t gapEnd) -> bool {
    // The invocation must be physically spelled in this materialized header gap
    // and owned by the include instance currently being rewritten.
    if (!m.invFile || m.invFile->empty() || !paths_.PathsEqual(*m.invFile, file))
      return false;
    if (!m.ownerIncludeId || *m.ownerIncludeId != ie.include->id)
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE)
      return false;
    if (*m.invB < gapBegin || gapEnd < *m.invE)
      return false;
    if (*m.invE > headerText.size())
      return false;
    // Byte-verify the recorded callsite text before preserving it.  This keeps
    // stale or ambiguous refold-map offsets from authorizing a source copy.
    if (m.invText &&
        StringRef(headerText).slice(*m.invB, *m.invE) != *m.invText)
      return false;

    return headerMacroInvocationIsSourceNeutralZeroToken(m);
  };

  auto headerIncludeIsDescendantOf =
      [&](const RefoldModel::IncludeItem &candidate,
          const RefoldModel::IncludeItem &root) -> bool {
    // Walk producer include-parent links rather than inferring ancestry from
    // paths.  Multiple include instances of the same file may exist, and the
    // preservation proof is instance-specific.
    std::optional<uint64_t> cur = candidate.parent;
    while (cur) {
      if (*cur == root.id)
        return true;
      const RefoldModel::IncludeItem *parent = nullptr;
      for (const auto &inc : model_.GetIncludes())
        if (inc.id == *cur) {
          parent = &inc;
          break;
        }
      if (!parent)
        return false;
      cur = parent->parent;
    }
    return false;
  };

  auto headerZeroTokenChildIncludeIsPreservableGap =
      [&](const RefoldModel::IncludeItem &child, uint64_t gapBegin,
          uint64_t gapEnd, StringRef replacement) -> bool {
    // Preserve only complete direct child include directives spelled in the
    // current parent header gap.  Nested descendants are checked below as part
    // of proving that the entire child include subtree is token-neutral.
    if (!child.parent || *child.parent != ie.include->id)
      return false;
    if (!paths_.PathsEqual(child.sitePath, file))
      return false;
    if (child.siteB >= child.siteE || child.siteE > headerText.size())
      return false;
    if (child.siteB < gapBegin || gapEnd < child.siteE)
      return false;
    if (child.cover.IsValid())
      return false;
    if (child.text.empty() ||
        StringRef(headerText).slice(child.siteB, child.siteE) != child.text)
      return false;

    // A zero-token include subtree may contain nested include directives, but
    // none of those descendants may contribute materialized PP tokens.
    for (const auto &inc : model_.GetIncludes()) {
      if (&inc == &child)
        continue;
      if (!headerIncludeIsDescendantOf(inc, child))
        continue;
      if (inc.cover.IsValid())
        return false;
    }

    for (const auto &macro : model_.GetMacroInvocations()) {
      // Macro calls inside the include subtree are allowed only when they are
      // themselves zero-token.  Any expansion, paste, or stringify output means
      // the include is not a neutral gap owner.
      if (!macro.ownerIncludeId)
        continue;
      bool ownedByChild = *macro.ownerIncludeId == child.id;
      if (!ownedByChild)
        for (const auto &inc : model_.GetIncludes())
          if (inc.id == *macro.ownerIncludeId &&
              headerIncludeIsDescendantOf(inc, child)) {
            ownedByChild = true;
            break;
          }
      if (ownedByChild &&
          RefoldSourceNeutralityProof::MacroInvocationHasMaterializedPPTokens(
              macro))
        return false;
    }

    for (const auto &group : model_.GetConds()) {
      // Conditional groups inside the include subtree are acceptable only if no
      // selected arm produced PP material.  Otherwise preserving the include as
      // a zero-token gap would silently discard live conditional output.
      if (!group.parentIncludeId)
        continue;
      bool ownedByChild = *group.parentIncludeId == child.id;
      if (!ownedByChild)
        for (const auto &inc : model_.GetIncludes())
          if (inc.id == *group.parentIncludeId &&
              headerIncludeIsDescendantOf(inc, child)) {
            ownedByChild = true;
            break;
          }
      if (!ownedByChild)
        continue;
      for (const RefoldModel::CondArm &arm : group.arms)
        if (arm.span && arm.span->IsValid() && arm.span->begin < arm.span->end)
          return false;
    }

    for (const auto &directive : model_.GetMacroDirectives()) {
      // Macro-state transitions in a preserved zero-token include become
      // visible after the replacement payload is emitted.  Reject the
      // preservation if the B-derived replacement text mentions that macro
      // name, because then moving the transition could change how the
      // replacement preprocesses.
      if (!directive.ownerIncludeId)
        continue;
      bool ownedByChild = *directive.ownerIncludeId == child.id;
      if (!ownedByChild)
        for (const auto &inc : model_.GetIncludes())
          if (inc.id == *directive.ownerIncludeId &&
              headerIncludeIsDescendantOf(inc, child)) {
            ownedByChild = true;
            break;
          }
      if (!ownedByChild)
        continue;
      if (macroStateProof_.ReplacementObservesMacroStateDirective(
              directive, replacement, /*unparseableObserves=*/false))
        return false;
    }

    return true;
  };

  headerIncludeIsSourceNeutralZeroToken =
      [&](const RefoldModel::IncludeItem &child) -> bool {
    // Header conditional islands are preserved verbatim, so an include inside
    // a selected arm is admissible when the ordinary header include-gap proof
    // can prove the complete child include subtree contributes no PP material.
    // Use the full materialized header as the gap surface here: the containing
    // conditional proof has already checked that the include site lies inside
    // the preserved group.
    return headerZeroTokenChildIncludeIsPreservableGap(
        child, 0, headerText.size(), StringRef());
  };


  auto ppSpanInsideHeaderMaterial = [&](const RefoldModel::PPSpan &span,
                                        uint64_t materialBeginA,
                                        uint64_t materialEndA) {
    if (!span.IsValid() || span.begin >= span.end)
      return true;
    return materialBeginA <= span.begin && span.end <= materialEndA;
  };

  auto headerArmIsSameOrNestedUnder = [&](const RefoldModel::ArmRef &candidate,
                                          uint64_t ancestorArmId) {
    if (!candidate.group || !candidate.arm)
      return false;
    if (candidate.arm->id == ancestorArmId)
      return true;

    // FindArmRefAtPP() reports the innermost selected arm that owns a token.
    // When proving that an outer header conditional group is wholly consumed,
    // tokens in nested selected arms still count as effective material of the
    // outer arm. Walk parent-arm links until either the queried arm is found or
    // the chain leaves the nested conditional tree.
    const RefoldModel::CondGroup *group = candidate.group;
    while (group && group->parentArmId) {
      if (*group->parentArmId == ancestorArmId)
        return true;
      std::optional<RefoldModel::ArmRef> parent =
          model_.GetArmRefById(*group->parentArmId);
      if (!parent || !parent->group)
        break;
      group = parent->group;
    }
    return false;
  };

  auto headerSelectedArmEffectiveMaterialInside =
      [&](const RefoldModel::CondArm &arm, uint64_t materialBeginA,
          uint64_t materialEndA) {
    bool sawEffectiveMaterial = false;
    for (uint64_t pp = 0; pp < aToks_.size(); ++pp) {
      std::optional<RefoldModel::ArmRef> owner = model_.FindArmRefAtPP(pp);
      if (!owner || !headerArmIsSameOrNestedUnder(*owner, arm.id))
        continue;

      sawEffectiveMaterial = true;
      if (pp < materialBeginA || pp >= materialEndA)
        return false;
    }

    // Prefer effective PP ownership when the map can answer it, because the
    // direct arm span may omit include- or macro-produced tokens controlled by
    // this arm. Fall back to the direct span only for older/sparse maps.
    if (sawEffectiveMaterial)
      return true;
    return arm.span && arm.span->IsValid() && arm.span->begin < arm.span->end &&
           ppSpanInsideHeaderMaterial(*arm.span, materialBeginA, materialEndA);
  };

  auto headerConditionalGroupIsConsumedSourceEnvelope =
      [&](const RefoldModel::CondGroup &group, uint64_t materialBeginA,
          uint64_t materialEndA) {
    if (!paths_.PathsEqual(group.file, file))
      return false;
    if (!group.parentIncludeId || *group.parentIncludeId != ie.include->id)
      return false;
    if (group.groupB >= group.groupE || group.groupE > headerText.size())
      return false;

    // This is the source-bearing counterpart of the zero-token conditional gap
    // proof.  The complete directive group may become a source-envelope piece
    // only when the selected arm's effective PP material is wholly consumed by
    // the A-side replacement material.  Otherwise copying/removing the group
    // would move surviving tokens across conditional-control structure.
    for (const RefoldModel::CondArm &arm : group.arms) {
      if (!arm.selected)
        continue;
      if (headerSelectedArmEffectiveMaterialInside(arm, materialBeginA,
                                                  materialEndA))
        return true;
    }
    return false;
  };

  auto headerMacroInvocationIsConsumedSourceEnvelope =
      [&](const RefoldModel::MacroInvocation &m, uint64_t materialBeginA,
          uint64_t materialEndA) {
    if (!m.invFile || m.invFile->empty() || !paths_.PathsEqual(*m.invFile, file))
      return false;
    if (!m.ownerIncludeId || *m.ownerIncludeId != ie.include->id)
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE ||
        *m.invE > headerText.size()) {
      return false;
    }
    if (m.invText &&
        StringRef(headerText).slice(*m.invB, *m.invE) != *m.invText) {
      return false;
    }

    bool sawMaterializedSpan = false;
    auto checkSpan = [&](const RefoldModel::PPSpan &span) {
      if (!span.IsValid() || span.begin >= span.end)
        return true;
      sawMaterializedSpan = true;
      return ppSpanInsideHeaderMaterial(span, materialBeginA, materialEndA);
    };

    // A source-bearing macro callsite is consumable only when every PP range
    // the producer attributes to the invocation is covered by the replacement
    // material. This prevents deleting a callsite whose expansion still has
    // surviving output elsewhere in B.
    for (const auto &span : m.spans)
      if (!checkSpan(span))
        return false;
    for (const auto &span : m.bodySpans)
      if (!checkSpan(span))
        return false;
    for (const auto &span : m.argSpans)
      if (!checkSpan(span))
        return false;
    for (const auto &span : m.stringifySpans)
      if (!checkSpan(span))
        return false;
    for (const auto &span : m.pasteSpans)
      if (!checkSpan(span))
        return false;

    return sawMaterializedSpan;
  };

  auto headerMacroInvocationOverlapsMaterial =
      [&](const RefoldModel::MacroInvocation &m, uint64_t materialBeginA,
          uint64_t materialEndA) {
    if (!m.invFile || m.invFile->empty() || !paths_.PathsEqual(*m.invFile, file))
      return false;
    if (!m.ownerIncludeId || *m.ownerIncludeId != ie.include->id)
      return false;
    if (!m.cover.IsValid() || m.cover.begin >= m.cover.end)
      return false;
    return materialBeginA < m.cover.end && m.cover.begin < materialEndA;
  };

  auto headerConditionalGroupSelectedMaterialOverlaps =
      [&](const RefoldModel::CondGroup &group, uint64_t materialBeginA,
          uint64_t materialEndA) {
    if (!paths_.PathsEqual(group.file, file))
      return false;
    if (!group.parentIncludeId || *group.parentIncludeId != ie.include->id)
      return false;
    for (const RefoldModel::CondArm &arm : group.arms) {
      if (!arm.selected || !arm.span || !arm.span->IsValid() ||
          arm.span->begin >= arm.span->end)
        continue;
      if (materialBeginA < arm.span->end && arm.span->begin < materialEndA)
        return true;
    }
    return false;
  };

  auto headerLineDirectiveStartsAtPrefix = [&](uint64_t pos) -> bool {
    if (pos >= headerText.size())
      return true;
    if (stringutils::isBOL(StringRef(headerText), static_cast<size_t>(pos)))
      return true;

    size_t lineStart = static_cast<size_t>(pos);
    while (lineStart > 0 && headerText[lineStart - 1] != '\n')
      --lineStart;
    return stringutils::isIndentOnly(StringRef(headerText), lineStart,
                                     static_cast<size_t>(pos));
  };

  auto canStartHeaderLineDirectiveWithOptionalLeadingNewline =
      [&](uint64_t pos) -> bool {
    if (headerLineDirectiveStartsAtPrefix(pos))
      return true;
    return pos == 0 || headerText[pos - 1] != '\\';
  };

  auto replacementHeaderSuffixBoundaryAllowsDirectiveLine =
      [&](StringRef replacement, uint64_t sourceEnd) {
    if (sourceEnd > headerText.size())
      return false;

    // Do not insert a directive after a line splice or trailing backslash: the
    // `#` would be joined to the previous logical source line instead of
    // starting a standalone preprocessing directive.
    if (!replacement.empty()) {
      if (replacement.back() == '\n') {
        if (stringutils::isLineSplice(replacement, replacement.size() - 1))
          return false;
      } else if (replacement.back() == '\\') {
        return false;
      }
    }

    // Whitespace on either side is already a lexical separator.  Otherwise,
    // fall back to token-boundary checking so widening the edit cannot glue two
    // neighboring tokens into a different token spelling.
    if (replacement.empty() || stringutils::isWs(replacement.back()))
      return true;
    if (sourceEnd >= headerText.size() ||
        stringutils::isWs(headerText[sourceEnd]))
      return true;

    std::optional<RefoldLexBoundaryToken> leftTok =
        refoldLastLexToken(replacement, lexLang_);
    std::optional<RefoldLexBoundaryToken> rightTok =
        refoldFirstLexToken(StringRef(headerText).drop_front(sourceEnd), lexLang_);
    if (!leftTok || !rightTok)
      return true;

    return !refoldNeedsLexicalSeparator(*leftTok, *rightTok, lexLang_);
  };

  auto activeHeaderDefinitionAtByte =
      [&](const RefoldModel::MacroDirective &definition, StringRef macroName,
          uint64_t offset) {
        // The carry proof applies only to the definition that is active at the
        // replacement start. Re-emitting a shadowed definition after the B
        // payload would synthesize a macro state that never existed at this
        // point in the original header.
        const RefoldModel::MacroDirective *active = nullptr;
        uint64_t activeEnd = 0;
        for (const auto &candidate : model_.GetMacroDirectives()) {
          std::optional<HeaderMacroStateDirectivePiece> piece =
              headerMacroStateDirectiveInterval(candidate);
          if (!piece || piece->end > offset)
            continue;
          if (StringRef(piece->name) != macroName)
            continue;
          if (!active || piece->end > activeEnd ||
              (piece->end == activeEnd && candidate.id > active->id)) {
            active = &candidate;
            activeEnd = piece->end;
          }
        }
        return active == &definition && definition.subkind == "#define";
      };

  auto headerRangeOverlapsStagedEdit = [&](uint64_t begin, uint64_t end) {
    // Do not carry directive text that is already being rewritten by another
    // staged header edit. A directive can be moved only when its original bytes
    // are still intact and owned by this single materialization proof.
    for (const TextEdit &edit : plan.edits)
      if (begin < edit.end && end > edit.start)
        return true;
    return false;
  };

  auto tokenRangesHaveSameSpelling = [&](uint64_t aBegin, uint64_t aEnd,
                                           uint64_t bBegin, uint64_t bEnd) {
    if (aEnd < aBegin || bEnd < bBegin ||
        (aEnd - aBegin) != (bEnd - bBegin))
      return false;
    if (aEnd > static_cast<uint64_t>(aToks_.size()) ||
        bEnd > static_cast<uint64_t>(bToks_.size()))
      return false;
    for (uint64_t i = 0; i < aEnd - aBegin; ++i)
      if (aToks_[static_cast<size_t>(aBegin + i)].spelling !=
          bToks_[static_cast<size_t>(bBegin + i)].spelling)
        return false;
    return true;
  };

  auto includeOwnsDirective = [&](const RefoldModel::MacroDirective &directive,
                                  const RefoldModel::IncludeItem &root) {
    if (!directive.ownerIncludeId)
      return false;
    if (*directive.ownerIncludeId == root.id)
      return true;
    for (const auto &inc : model_.GetIncludes())
      if (inc.id == *directive.ownerIncludeId)
        return headerIncludeIsDescendantOf(inc, root);
    return false;
  };

  auto recordedMacroDirectiveMatchesOwnerFile =
      [&](const RefoldModel::MacroDirective &directive) {
    const RefoldModel::IncludeItem *owner = nullptr;
    if (!directive.ownerIncludeId)
      return false;
    for (const auto &inc : model_.GetIncludes())
      if (inc.id == *directive.ownerIncludeId) {
        owner = &inc;
        break;
      }
    if (!owner)
      return false;

    const std::string ownerPath = refoldIncludeEnteredFileSpelling(*owner);
    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerPath));
    if (!bufOrErr)
      return false;
    const MemoryBuffer &mb = **bufOrErr;
    StringRef ownerBytes(mb.getBufferStart(), mb.getBufferSize());
    return macroStateProof_.RecoverMacroStateDirectiveLineInterval(
               directive, ownerPath, ownerBytes, directive.ownerIncludeId)
        .has_value();
  };

  auto collectMacroStatePreservationsFromIncludeSubtree =
      [&](const RefoldModel::IncludeItem &root, StringRef replacement,
          SmallVectorImpl<HeaderPreservedGapPiece> &out) {
    SmallVector<HeaderPreservedGapPiece, 4> pieces;
    for (const auto &directive : model_.GetMacroDirectives()) {
      if (directive.subkind != "#define" && directive.subkind != "#undef")
        continue;
      if (!includeOwnsDirective(directive, root))
        continue;

      if (macroStateProof_.ReplacementObservesMacroStateDirective(
              directive, replacement, /*unparseableObserves=*/true))
        return false;
      if (!recordedMacroDirectiveMatchesOwnerFile(directive))
        return false;

      HeaderPreservedGapPiece piece;
      piece.kind = HeaderPreservedGapPiece::Kind::MacroStateDirective;
      piece.directive = &directive;
      piece.id = directive.id;
      pieces.push_back(std::move(piece));
    }

    llvm::sort(pieces, [](const HeaderPreservedGapPiece &lhs,
                          const HeaderPreservedGapPiece &rhs) {
      return lhs.id < rhs.id;
    });
    out.append(pieces.begin(), pieces.end());
    return true;
  };

  auto tryBuildConsumedHeaderConditionalCoalescedPatch =
      [&](size_t idx, IncludePatch &coalesced, size_t &skipThrough) {
    if (idx + 1 >= ie.patches.size())
      return false;

    const IncludePatch &first = ie.patches[idx];
    if (first.hasDirectHeaderByteRange)
      return false;
    if (first.aStart >= first.aEnd)
      return false;

    // Some LCS shapes preserve a token from inside a selected conditional arm
    // (for example the '=' in `int middle = 99;`) while adjacent hunks delete
    // the rest of that arm.  Per-patch materialization cannot prove the whole
    // #if/#endif envelope because no individual patch covers the selected arm.
    // Merge the shortest adjacent patch run that does cover a complete selected
    // header conditional group, then let the normal header source-envelope
    // proof consume the group as one source piece.
    for (size_t endIdx = idx + 1; endIdx < ie.patches.size(); ++endIdx) {
      const IncludePatch &last = ie.patches[endIdx];
      if (last.include != first.include)
        break;
      if (last.ownerHasCondArmCert != first.ownerHasCondArmCert)
        break;
      if (last.ownerHasCondArmCert &&
          last.ownerCondArmIdCert != first.ownerCondArmIdCert)
        break;
      if (last.aStart >= last.aEnd || last.aEnd <= first.aStart)
        break;
      if (last.bEnd < first.bStart)
        break;

      const uint64_t materialBeginA = first.aStart;
      const uint64_t materialEndA = last.aEnd;
      bool consumesConditionalGroup = false;
      bool overlapsConditionalGroup = false;
      for (const auto &group : model_.GetConds()) {
        if (!headerConditionalGroupSelectedMaterialOverlaps(
                group, materialBeginA, materialEndA))
          continue;
        overlapsConditionalGroup = true;
        if (headerConditionalGroupIsConsumedSourceEnvelope(
                group, materialBeginA, materialEndA)) {
          consumesConditionalGroup = true;
          break;
        }
      }

      if (overlapsConditionalGroup && !consumesConditionalGroup)
        continue;
      if (!consumesConditionalGroup)
        continue;

      coalesced = first;
      coalesced.aEnd = materialEndA;
      coalesced.bEnd = last.bEnd;
      coalesced.insertBytes = refoldSliceExactTokenCoverage(
                                  bTokOff_, bToks_, bSource_, coalesced.bStart,
                                  coalesced.bEnd)
                                  .str();
      skipThrough = endIdx;
      return true;
    }

    return false;
  };

  auto tryBuildMaterialChildStateCoalescedPatch =
      [&](size_t idx, IncludePatch &coalesced, size_t &skipThrough) {
    if (idx + 1 >= ie.patches.size())
      return false;

    const IncludePatch &first = ie.patches[idx];
    const IncludePatch &second = ie.patches[idx + 1];
    if (first.hasDirectHeaderByteRange || second.hasDirectHeaderByteRange)
      return false;
    if (first.include != second.include)
      return false;
    if (first.ownerHasCondArmCert != second.ownerHasCondArmCert)
      return false;
    if (first.ownerHasCondArmCert &&
        first.ownerCondArmIdCert != second.ownerCondArmIdCert)
      return false;
    if (first.aStart >= first.aEnd || second.aStart >= second.aEnd)
      return false;
    if (second.aEnd <= first.aStart || second.bEnd < first.bStart)
      return false;

    for (const auto &child : model_.GetIncludes()) {
      if (!child.parent || *child.parent != ie.include->id ||
          !paths_.PathsEqual(child.sitePath, file) || !child.cover.IsValid())
        continue;
      if (child.cover.end <= child.cover.begin)
        continue;

      if (!(first.aStart < child.cover.begin &&
            child.cover.begin < first.aEnd &&
            first.aEnd <= second.aStart &&
            second.aStart < child.cover.end &&
            child.cover.end < second.aEnd))
        continue;

      uint64_t prefix = 0;
      while (first.aStart + prefix < child.cover.begin &&
             first.bStart + prefix < second.bEnd &&
             tokenRangesHaveSameSpelling(first.aStart + prefix,
                                         first.aStart + prefix + 1,
                                         first.bStart + prefix,
                                         first.bStart + prefix + 1)) {
        ++prefix;
      }

      if (first.aStart + prefix >= child.cover.begin ||
          first.bStart + prefix > second.bEnd)
        continue;

      const uint64_t newAStart = first.aStart + prefix;
      const uint64_t newBStart = first.bStart + prefix;
      std::string newInsert = refoldSliceExactTokenCoverage(
                                  bTokOff_, bToks_, bSource_, newBStart,
                                  second.bEnd)
                                  .str();

      SmallVector<HeaderPreservedGapPiece, 4> statePieces;
      if (!collectMacroStatePreservationsFromIncludeSubtree(
              child, StringRef(newInsert), statePieces) ||
          statePieces.empty())
        continue;

      coalesced = first;
      coalesced.aStart = newAStart;
      coalesced.aEnd = second.aEnd;
      coalesced.bStart = newBStart;
      coalesced.bEnd = second.bEnd;
      coalesced.insertBytes = std::move(newInsert);
      skipThrough = idx + 1;
      return true;
    }

    return false;
  };

  DenseSet<size_t> skippedIncludePatchIndices;

  for (size_t idx = 0; idx < ie.patches.size(); ++idx) {
    if (skippedIncludePatchIndices.count(idx))
      continue;

    IncludePatch coalescedPatch = ie.patches[idx];
    size_t skipThrough = idx;
    const IncludePatch *patchPtr = &ie.patches[idx];
    if (tryBuildConsumedHeaderConditionalCoalescedPatch(idx, coalescedPatch,
                                                       skipThrough) ||
        tryBuildMaterialChildStateCoalescedPatch(idx, coalescedPatch,
                                                 skipThrough)) {
      patchPtr = &coalescedPatch;
      for (size_t skipIdx = idx + 1; skipIdx <= skipThrough; ++skipIdx)
        skippedIncludePatchIndices.insert(skipIdx);
    }

    const IncludePatch &p = *patchPtr;

    const bool isInsert = (p.aStart == p.aEnd) && (p.bStart < p.bEnd);
    const bool isDelete = (p.aStart < p.aEnd) && (p.bStart == p.bEnd);
    const bool isReplace = (p.aStart < p.aEnd) && (p.bStart < p.bEnd);


    if (!isInsert && !isDelete && !isReplace) {
      // Ignore empty or malformed patches defensively.
      continue;
    }

    if (p.hasDirectHeaderByteRange) {
      if (p.directHeaderByteBegin > p.directHeaderByteEnd ||
          p.directHeaderByteEnd > fileLen) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("direct include layout patch[{0}] has invalid "
                          "header byte range [{1},{2}) for file {3}",
                          idx, p.directHeaderByteBegin, p.directHeaderByteEnd,
                          file)
                .str();
        return plan;
      }

      IncludeAnchorWitness directWitness;
      directWitness.evidence = IncludeAnchorEvidenceKind::MappedHeaderTokens;
      directWitness.hasByteRange = true;
      directWitness.startByte = p.directHeaderByteBegin;
      directWitness.endByte = p.directHeaderByteEnd;
      directWitness.hasFirstPP = p.aStart < p.aEnd;
      directWitness.firstPP = p.aStart;
      directWitness.hasLastPP = p.aStart < p.aEnd;
      directWitness.lastPP = p.aStart < p.aEnd ? p.aEnd - 1 : p.aStart;


      TextEdit edit{p.directHeaderByteBegin, p.directHeaderByteEnd,
                    p.insertBytes, std::nullopt, std::nullopt, {}, {}, {}};
      textEditAssembler_.AttachAcceptedResultCarrier(
          edit, proofLattice_.BuildAcceptedIncludeCandidate(
                    AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens, p,
                    &directWitness));
      plan.edits.push_back(std::move(edit));
      continue;
    }

    // Decide which logical header decl "owns" this patch, if any.
    const auto *decl = FindHeaderDeclForPatch(*ie.include, p);

    // Effective PP coverage in this header we're allowed to touch.
    // For INSERTs we deliberately work at include scope so that inserts that
    // land exactly at a declaration boundary (for example, between
    // `int yyy(...);` and `int zzz(...);`) can anchor to the first token
    // of the following declaration rather than being forced back inside the
    // previous one. For DELETE / REPLACE we restrict to the owning decl.
    uint64_t ppLo = coverBegin;
    uint64_t ppHi = coverEnd;
    if (!isInsert && decl) {
      ppLo = std::max(ppLo, decl->span.begin);
      ppHi = std::min(ppHi, decl->span.end);
    }
    ppHi = std::max(ppHi, ppLo);


    std::optional<uint64_t> startByte;
    std::optional<uint64_t> endByte;
    IncludeAnchorWitness mappedHeaderWitness;
    SmallVector<HeaderPreservedGapPiece, 4> headerGapPreservations;
    std::optional<SourceLineDirectiveGapResume> headerSourceLineDirectiveResume;
    bool usedFullHeaderEnvelope = false;

    const auto &tokmapByPP = model_.GetTokmapByPP();

    uint64_t materialAStart = p.aStart;
    uint64_t materialAEnd = p.aEnd;
    uint64_t materialBStart = p.bStart;
    uint64_t materialBEnd = p.bEnd;
    uint64_t materialInsertPos = p.aStart;
    std::string materialInsertBytes = p.insertBytes;

    // Return true when an owner-local byte gap contains source-authored
    // line-control state.  This is intentionally a syntactic gap predicate: the
    // full logical-state evaluator lives in LineDirectiveInserter, but here we
    // only need to know whether an LCS boundary-token tie would cross a gap
    // that can change the logical location of the copied suffix.
    auto sourceIntervalContainsLineControlDirective =
        [&](uint64_t begin, uint64_t end) -> bool {
      if (begin >= end || end > headerText.size())
        return false;

      uint64_t lineBegin = begin;
      while (lineBegin < end) {
        std::string logicalLine;
        uint64_t afterLine = lineBegin;
        if (!collectLineSpliceLogicalLine(headerText, lineBegin, end, logicalLine,
                                      afterLine))
          return false;
        if (afterLine <= lineBegin)
          return false;

        StringRef rest = StringRef(logicalLine).ltrim(" \t\v\f");
        if (rest.consume_front("#")) {
          rest = rest.ltrim(" \t\v\f");
          if (rest.starts_with("line") &&
              (rest.size() == StringRef("line").size() ||
               !stringutils::isIdentPart(rest[StringRef("line").size()])))
            return true;
          if (!rest.empty() && '0' <= rest.front() && rest.front() <= '9')
            return true;
        }

        lineBegin = afterLine;
      }

      return false;
    };

    if (isInsert) {
      // Local repair for a duplicated boundary-token LCS tie.
      //
      // Shape:
      //   A:  <owner token T>  <source-only gap>  <next owner token>
      //   B:  T <inserted payload ending with another T> <next owner token>
      //
      // A token-level LCS may match the original source token T to the later B
      // copy after the insertion island.  For ordinary gaps this is usually just
      // another valid spelling of the same token edit.  Across a source-only
      // line-control gap, however, that tie changes where the inserted payload
      // sits relative to the logical-location state that the copied suffix will
      // observe.  The source token must remain before the gap; the inserted
      // payload must be emitted after the gap, followed by normal #line resync.
      //
      // This normalization is deliberately local to include-owned insertions and
      // only fires when the existing A->B token map proves the exact duplicate
      // boundary-token shape.  It does not mutate the global LCS, owner choice,
      // or any sideband/macro proof.
      auto normalizeDuplicatedBoundaryTokenAcrossLineControlGap = [&]() {
        if (materialAStart >= static_cast<uint64_t>(aToks_.size()))
          return;
        if (materialBStart >= materialBEnd ||
            materialBEnd >= static_cast<uint64_t>(bToks_.size()))
          return;
        if (abTokMapA2B_.empty() ||
            materialAStart >= static_cast<uint64_t>(abTokMapA2B_.size()))
          return;
        // The original A boundary token is currently matched to the B token
        // immediately after the insertion island.  Without this witness, there
        // is no duplicated-boundary-token tie to normalize.
        if (abTokMapA2B_[static_cast<size_t>(materialAStart)] !=
            static_cast<int64_t>(materialBEnd))
          return;
        if (aToks_[static_cast<size_t>(materialAStart)].spelling !=
                bToks_[static_cast<size_t>(materialBStart)].spelling ||
            aToks_[static_cast<size_t>(materialAStart)].spelling !=
                bToks_[static_cast<size_t>(materialBEnd)].spelling)
          return;

        std::optional<uint64_t> nextOwnerPP;
        for (uint64_t pp = materialAStart + 1; pp < ppHi; ++pp) {
          auto it = tokmapByPP.find(pp);
          if (it != tokmapByPP.end() && paths_.PathsEqual(it->second.file, file)) {
            nextOwnerPP = pp;
            break;
          }
        }
        if (!nextOwnerPP)
          return;

        // Examine the owner-local source bytes between the retained source
        // boundary token and the next copied owner token.  Only a source
        // line-control directive makes this token tie semantically observable
        // for placement; otherwise the normal token-envelope insertion is fine.
        std::optional<uint64_t> leftEnd =
            ByteEndForPPInFile(file, materialAStart);
        std::optional<uint64_t> rightBegin =
            ByteStartForPPInFile(file, *nextOwnerPP);
        if (!leftEnd || !rightBegin || *rightBegin < *leftEnd)
          return;
        if (!sourceIntervalContainsLineControlDirective(*leftEnd, *rightBegin))
          return;

        const uint64_t normalizedBStart = materialBStart + 1;
        const uint64_t normalizedBEnd = materialBEnd + 1;
        if (normalizedBStart >= normalizedBEnd ||
            normalizedBEnd > static_cast<uint64_t>(bTokOff_.size()))
          return;

        // Drop the duplicated leading B token from the island and include the
        // later B token that closed the island.  The insertion point advances
        // to the next owner token so the source line-control gap remains in
        // place before the inserted payload.
        materialBStart = normalizedBStart;
        materialBEnd = normalizedBEnd;
        materialInsertPos = *nextOwnerPP;
        // Use the B token *envelope*, not exact token coverage.  The envelope
        // preserves the whitespace/newline between the inserted payload and the
        // copied suffix; losing that newline would concatenate C tokens even
        // though the token interval proof is otherwise correct.
        materialInsertBytes =
            refoldSliceTokenEnvelope(bTokOff_, bSource_, materialBStart, materialBEnd)
                .str();
      };

      normalizeDuplicatedBoundaryTokenAcrossLineControlGap();

      std::optional<std::pair<uint64_t, uint64_t>> bBytes =
          sourceMapper_.BTokenRangeToByteRange(materialBStart, materialBEnd);
      materialInsertBytes = textEditAssembler_.StripSeparatelyOwnedSidebandReplay(
          materialInsertBytes,
          bBytes ? std::optional<uint64_t>(bBytes->first) : std::nullopt,
          bBytes ? std::optional<uint64_t>(bBytes->second) : std::nullopt);
    }

    // If a replacement hunk starts by re-emitting the exact tokens of a
    // complete child include, keep that include as the survivor and remove the
    // matching prefix from the materialized replacement. This is the header
    // analogue of preferring the selected-arm include owner in mixed
    // TU/include closures: identical duplicate tokens should not force us to
    // replace a preserved include with raw B bytes.
    if (isReplace) {
      bool strippedPrefix = true;
      while (strippedPrefix && materialAStart < materialAEnd &&
             materialBStart < materialBEnd) {
        strippedPrefix = false;
        for (const auto &child : model_.GetIncludes()) {
          if (!child.parent || *child.parent != ie.include->id ||
              !paths_.PathsEqual(child.sitePath, file) || !child.cover.IsValid())
            continue;
          if (child.cover.begin != materialAStart ||
              child.cover.end > materialAEnd)
            continue;

          const uint64_t childTokenCount = child.cover.end - child.cover.begin;
          if (materialBStart + childTokenCount > materialBEnd)
            continue;
          if (!tokenRangesHaveSameSpelling(child.cover.begin, child.cover.end,
                                           materialBStart,
                                           materialBStart + childTokenCount))
            continue;

          materialAStart = child.cover.end;
          materialBStart += childTokenCount;
          materialInsertBytes =
              refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                      materialBStart, materialBEnd)
                  .str();
          strippedPrefix = true;
          break;
        }
      }
    }

    if (isInsert) {
      // INSERT: interpret the normalized A-position as "before the next token"
      // in this header.  Most insertions use the original hunk A gap.  The
      // duplicate-boundary-token normalization above may advance this position
      // across a source-only line-control gap while keeping the original token
      // in place.
      const uint64_t pos = materialInsertPos;
      auto anchorMatchesCondArmCert = [&](uint64_t anchorByte) -> bool {
        if (!p.ownerHasCondArmCert)
          return true;
        auto armRef =
            model_.FindArmRefForByte(file, ie.include->id, anchorByte);
        return armRef && armRef->arm && armRef->arm->id == p.ownerCondArmIdCert;
      };

      struct InsertAnchorCandidate {
        AcceptedPathKind path = AcceptedPathKind::Unknown;
        IncludeAnchorWitness witness;
        uint64_t anchorByte = 0;
      };

      struct SelectedInsertAnchorCandidate {
        InsertAnchorCandidate anchor;
        AcceptedResultCandidate accepted;
      };

      // Select the best theorem-facing insertion anchor by wrapping each local
      // anchor candidate in the accepted-result carrier and letting the shared
      // proof-discharge/lattice selector choose among them.  Return the
      // accepted carrier together with the path-local anchor, so the commit
      // path does not have to rediscover or rebuild the proof that won.
      auto selectBestInsertCandidate =
          [&](ArrayRef<InsertAnchorCandidate> candidates)
          -> std::optional<SelectedInsertAnchorCandidate> {
        if (candidates.empty())
          return std::nullopt;

        SmallVector<AcceptedResultCandidate, 4> acceptedCandidates;
        acceptedCandidates.reserve(candidates.size());
        for (const InsertAnchorCandidate &candidate : candidates) {
          acceptedCandidates.push_back(proofLattice_.BuildAcceptedIncludeCandidate(
              candidate.path, p, &candidate.witness));
        }

        const std::optional<SelectedAcceptedResultCandidate> selected =
            proofLattice_.SelectPreferredAcceptedResultCandidate(acceptedCandidates);
        if (!selected) {
          return std::nullopt;
        }

        SelectedInsertAnchorCandidate result;
        result.anchor = candidates[selected->index];
        result.accepted = selected->candidate;
        return result;
      };

      // Materialize the selected insertion anchor as a header-local TextEdit,
      // applying boundary padding and line-resync handling before attaching the
      // exact accepted include-candidate carrier that won selection.
      auto commitInsertCandidate = [&](const SelectedInsertAnchorCandidate
                                           &selected) {
        const InsertAnchorCandidate &candidate = selected.anchor;
        std::string text = refoldPadAtBoundaries(
            headerText, static_cast<size_t>(candidate.anchorByte),
            static_cast<size_t>(candidate.anchorByte), materialInsertBytes,
            /*allowLeft=*/true, /*allowRight=*/true, lexLang_);

        auto hasCopiedHeaderSuffix = [&]() -> bool {
          for (uint64_t byte = candidate.anchorByte; byte < fileLen; ++byte)
            if (!stringutils::isWs(headerText[static_cast<size_t>(byte)]))
              return true;
          return false;
        };

        auto childIncludeAtBoundaryAnchor =
            [&]() -> const RefoldModel::IncludeItem * {
          for (const auto &child : model_.GetIncludes()) {
            if (!child.parent || *child.parent != ie.include->id ||
                !paths_.PathsEqual(child.sitePath, file))
              continue;
            if (child.siteB == candidate.anchorByte ||
                child.siteE == candidate.anchorByte)
              return &child;
          }
          return nullptr;
        };

        auto includeHasVisibleSidebandWork = [&](uint64_t includeId) -> bool {
          return llvm::any_of(sidebandPragmaEdits_,
                              [&](const SidebandPragmaEdit &sideband) {
            return sideband.TargetsInclude(includeId) &&
                   sideband.EmitsVisibleReplayText();
          });
        };

        auto suffixBeginsWithPreprocessorDirective = [&]() -> bool {
          size_t pos = static_cast<size_t>(candidate.anchorByte);
          if (pos >= headerText.size())
            return false;

          // Local line resync is still required before a preserved directive
          // line.  Unlike ordinary suffix tokens, directives such as #include,
          // #define, #undef, and #line observe their presumed source location
          // through preprocessing state and diagnostics.  Horizontal
          // indentation before the '#' belongs to the directive line and is a
          // valid flush point for the pending #line.
          size_t lineEnd = headerText.find('\n', pos);
          if (lineEnd == StringRef::npos)
            lineEnd = headerText.size();
          while (pos < lineEnd &&
                 (headerText[pos] == ' ' || headerText[pos] == '\t'))
            ++pos;
          return pos < lineEnd && headerText[pos] == '#';
        };

        auto replayContainsPreprocessorDirectiveLine =
            [](StringRef replay) -> bool {
          size_t lineBegin = 0;
          while (lineBegin < replay.size()) {
            size_t lineEnd = replay.find('\n', lineBegin);
            if (lineEnd == StringRef::npos)
              lineEnd = replay.size();

            size_t pos = lineBegin;
            while (pos < lineEnd &&
                   (replay[pos] == ' ' || replay[pos] == '\t'))
              ++pos;
            if (pos < lineEnd && replay[pos] == '#')
              return true;

            if (lineEnd == replay.size())
              break;
            lineBegin = lineEnd + 1;
          }
          return false;
        };

        const bool copiedHeaderSuffix = hasCopiedHeaderSuffix();
        const RefoldModel::IncludeItem *boundaryChild =
            childIncludeAtBoundaryAnchor();
        const bool boundaryChildWillBeMaterialized =
            boundaryChild && includeHasVisibleSidebandWork(boundaryChild->id);
        const bool suffixObservesLineState =
            lineControlProof_.OwnerSuffixHasLineStateSensitiveBuiltin(ie.include->id, file,
                                                    candidate.anchorByte);
        const bool suffixStartsDirective =
            suffixBeginsWithPreprocessorDirective();
        const bool replayCarriesDirectiveLine =
            replayContainsPreprocessorDirectiveLine(text);

        // A pure insertion at physical header EOF has no copied header suffix
        // whose logical location must be restored inside this owner.  For a
        // child-include boundary, suppress the local parent resync only when
        // the child is itself being materialized by a proved sideband replay;
        // the child wrapper then performs the transition immediately.  If the
        // child #include remains preserved, or if the next untouched suffix
        // line is any other preprocessing directive, restore the parent line
        // state before that directive.
        //
        // Two additional map-backed observations also require local resync:
        //  * a copied suffix macro invocation whose expansion contains
        //    __LINE__/__FILE__ through caller_macro_id evidence; and
        //  * an insertion payload that itself carries a preprocessing directive
        //    line, such as a sideband #pragma co-owned by the ordinary
        //    insertion island.  In both cases, the following copied header
        //    bytes must resume in the original owner file/line state.
        const bool needsLocalResync =
            copiedHeaderSuffix &&
            (suffixObservesLineState || suffixStartsDirective ||
             replayCarriesDirectiveLine) &&
            !boundaryChildWillBeMaterialized;
        TextEdit edit = needsLocalResync ? MakeTextEditWithResyncOrPending(
                                               headerText, candidate.anchorByte,
                                               candidate.anchorByte, text, file,
                                               ie.include->id)
                                         : TextEdit{candidate.anchorByte,
                                                    candidate.anchorByte,
                                                    std::move(text),
                                                    std::nullopt,
                                                    std::nullopt,
                                                    {},
                                                    {},
                                                    {}};
        textEditAssembler_.AttachAcceptedResultCarrier(edit, selected.accepted);
        plan.edits.push_back(std::move(edit));
      };

      // If this INSERT gap is exactly the begin of the currently selected
      // conditional arm in this header, treat it as the *boundary before* the
      // conditional group rather than as "before the next token" inside the
      // arm body. In preprocessed token space, both positions collapse onto the
      // same PP gap because the controlling directive itself contributes no PP
      // tokens. For include-owned pure insertions, the boundary policy is to
      // keep the insertion outside the conditional unless the insertion is
      // explicitly arm-owned.
      auto selectedArmBeginBoundaryByte = [&]() -> std::optional<uint64_t> {
        auto rightArmRef = model_.FindArmRefAtPP(pos);
        if (!rightArmRef || !rightArmRef->group || !rightArmRef->arm)
          return std::nullopt;
        if (!rightArmRef->arm->selected || !rightArmRef->arm->span)
          return std::nullopt;
        if (rightArmRef->arm->span->begin != pos)
          return std::nullopt;
        if (!paths_.PathsEqual(rightArmRef->group->file, file))
          return std::nullopt;
        if (!rightArmRef->group->parentIncludeId ||
            *rightArmRef->group->parentIncludeId != ie.include->id)
          return std::nullopt;

        auto leftArmRef = (pos > 0) ? model_.FindArmRefAtPP(pos - 1)
                                    : std::optional<RefoldModel::ArmRef>{};
        if (leftArmRef && leftArmRef->arm &&
            leftArmRef->arm->id == rightArmRef->arm->id)
          return std::nullopt;

        const bool explicitArmOwned =
            p.ownerHasCondArmCert &&
            p.ownerCondArmIdCert == rightArmRef->arm->id;
        if (explicitArmOwned)
          return std::nullopt;

        return std::clamp<uint64_t>(rightArmRef->group->groupB, 0ULL, fileLen);
      };

      {
        // First try the strongest insertion anchors: boundaries already tied to
        // the selected conditional arm or to a child include boundary.  The
        // child-boundary case is a declared include-preserving proof, not an
        // unclassified fallback branch: the witness names the direct child
        // include whose spelled directive supplies the anchor. These anchors
        // preserve include structure without relying on neighboring PP tokens.
        SmallVector<InsertAnchorCandidate, 2> topTierCandidates;

        if (const std::optional<uint64_t> insertByte =
                selectedArmBeginBoundaryByte()) {
          if (*insertByte <= fileLen && anchorMatchesCondArmCert(*insertByte)) {
            InsertAnchorCandidate candidate;
            candidate.path =
                AcceptedPathKind::IncludeInsertSelectedConditionalBoundary;
            candidate.anchorByte = *insertByte;
            candidate.witness.evidence =
                IncludeAnchorEvidenceKind::SelectedConditionalBoundary;
            candidate.witness.hasAnchorByte = true;
            candidate.witness.anchorByte = *insertByte;
            if (auto rightArmRef = model_.FindArmRefAtPP(pos);
                rightArmRef && rightArmRef->arm) {
              candidate.witness.hasCondArmId = true;
              candidate.witness.condArmId = rightArmRef->arm->id;
            }
            topTierCandidates.push_back(std::move(candidate));
          }
        }

        IncludeAnchorWitness childBoundaryWitness;
        if (const std::optional<uint64_t> insertByte =
                ComputeChildBoundaryInsertByte(p, file,
                                               &childBoundaryWitness)) {
          if (*insertByte <= fileLen && anchorMatchesCondArmCert(*insertByte)) {
            InsertAnchorCandidate candidate;
            candidate.path = AcceptedPathKind::IncludeInsertChildBoundary;
            candidate.anchorByte = *insertByte;
            candidate.witness = childBoundaryWitness;
            candidate.witness.hasAnchorByte = true;
            candidate.witness.anchorByte = *insertByte;
            topTierCandidates.push_back(std::move(candidate));
          }
        }

        if (auto selected = selectBestInsertCandidate(topTierCandidates)) {
          commitInsertCandidate(*selected);
          continue;
        }
      }

      // If no structural boundary anchor won, look for the nearest mapped PP
      // token to the right in the same file and anchor before it.
      std::optional<uint64_t> rightAnchorPP;
      for (uint64_t pp = std::max(pos, ppLo); pp < ppHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && paths_.PathsEqual(it->second.file, file)) {
          rightAnchorPP = pp;
          break;
        }
      }

      if (rightAnchorPP) {
        const std::optional<uint64_t> rightStartByte =
            ByteStartForPPInFile(file, *rightAnchorPP);
        IncludeAnchorWitness rightNeighborWitness;
        rightNeighborWitness.evidence =
            IncludeAnchorEvidenceKind::RightNeighborPP;
        rightNeighborWitness.hasNeighborPP = true;
        rightNeighborWitness.neighborPP = *rightAnchorPP;
        rightNeighborWitness.hasAnchorByte = rightStartByte.has_value();
        rightNeighborWitness.anchorByte =
            rightStartByte ? *rightStartByte : 0ULL;
        if (!rightStartByte) {
          plan.requiresIncludeRealization = true;
          plan.realizationReason =
              llvm::formatv("INSERT: failed to map anchorPP={0} in file {1}",
                            *rightAnchorPP, file)
                  .str();
          return plan;
        }
        if (anchorMatchesCondArmCert(*rightStartByte)) {
          InsertAnchorCandidate candidate;
          candidate.path = AcceptedPathKind::IncludeInsertRightNeighborPP;
          candidate.anchorByte = *rightStartByte;
          candidate.witness = rightNeighborWitness;
          SmallVector<InsertAnchorCandidate, 1> rightCandidates;
          rightCandidates.push_back(std::move(candidate));
          if (auto selected = selectBestInsertCandidate(rightCandidates)) {
            commitInsertCandidate(*selected);
            continue;
          }
        }
      }

      // Right-neighbor anchoring is preferred because it naturally inserts
      // before the next stable token. If unavailable, collect secondary
      // include-preserving anchors: the nearest mapped left neighbor, or the
      // containing declaration boundary when no neighbor exists. These are not
      // source-mapping fallbacks; each candidate still needs a concrete witness
      // before it can be selected.
      std::optional<uint64_t> leftAnchorPP;
      if (pos > ppLo && ppHi > ppLo) {
        for (uint64_t pp = std::min(pos - 1, ppHi - 1);; --pp) {
          auto it = tokmapByPP.find(pp);
          if (it != tokmapByPP.end() && paths_.PathsEqual(it->second.file, file)) {
            leftAnchorPP = pp;
            break;
          }
          if (pp == ppLo)
            break;
        }
      }

      SmallVector<InsertAnchorCandidate, 2> secondaryCandidates;
      if (leftAnchorPP) {
        const std::optional<uint64_t> leftStartByte =
            ByteEndForPPInFile(file, *leftAnchorPP);
        const bool insertionAtIncludeEOF = pos >= ppHi;
        // Insertion at the include cover end is an explicit left-neighbor EOF
        // anchor, not an unmapped PP->EOF source projection.  The proof still
        // requires `leftAnchorPP` to map into this file; only after that mapped
        // neighbor establishes owner-local suffix placement do we use the
        // physical file length as the concrete byte anchor for the zero-width
        // EOF insertion.
        const std::optional<uint64_t> leftAnchorByte =
            leftStartByte ? std::optional<uint64_t>(
                                insertionAtIncludeEOF ? fileLen
                                                      : *leftStartByte)
                          : std::nullopt;
        IncludeAnchorWitness leftNeighborWitness;
        leftNeighborWitness.evidence =
            IncludeAnchorEvidenceKind::LeftNeighborPP;
        leftNeighborWitness.hasNeighborPP = true;
        leftNeighborWitness.neighborPP = *leftAnchorPP;
        leftNeighborWitness.hasAnchorByte = leftAnchorByte.has_value();
        leftNeighborWitness.anchorByte =
            leftAnchorByte ? *leftAnchorByte : 0ULL;
        if (!leftAnchorByte) {
          plan.requiresIncludeRealization = true;
          plan.realizationReason =
              llvm::formatv("INSERT: failed to map left-neighbor anchorPP={0} "
                            "in file {1}",
                            *leftAnchorPP, file)
                  .str();
          return plan;
        }
        if (anchorMatchesCondArmCert(*leftAnchorByte)) {
          InsertAnchorCandidate candidate;
          candidate.path = AcceptedPathKind::IncludeInsertLeftNeighborPP;
          candidate.anchorByte = *leftAnchorByte;
          candidate.witness = leftNeighborWitness;
          secondaryCandidates.push_back(std::move(candidate));
        }
      } else if (decl) {
        // With no mapped PP neighbor, use the declaration boundary that owns
        // this patch. This keeps the insertion local to the header declaration
        // only when the declaration witness discharges; otherwise the caller
        // widens to include realization.
        const uint64_t declAnchorByte =
            std::clamp<uint64_t>(decl->headerE, 0ULL, fileLen);
        IncludeAnchorWitness declWitness;
        declWitness.evidence = IncludeAnchorEvidenceKind::DeclBoundary;
        declWitness.hasAnchorByte = true;
        declWitness.anchorByte = declAnchorByte;
        declWitness.hasDeclHeaderRange = true;
        declWitness.declHeaderB = decl->headerB;
        declWitness.declHeaderE = decl->headerE;
        if (anchorMatchesCondArmCert(declAnchorByte)) {
          InsertAnchorCandidate candidate;
          candidate.path = AcceptedPathKind::IncludeInsertDeclBoundary;
          candidate.anchorByte = declAnchorByte;
          candidate.witness = declWitness;
          secondaryCandidates.push_back(std::move(candidate));
        }
      }

      if (auto selected = selectBestInsertCandidate(secondaryCandidates)) {
        commitInsertCandidate(*selected);
        continue;
      }

      // No include-preserving insertion anchor discharged. Switch to the
      // explicit include-realization path instead of manufacturing a weaker
      // insertion witness.
      plan.requiresIncludeRealization = true;
      plan.realizationReason =
          llvm::formatv("INSERT: cannot anchor include patch in file {0} "
                        "(no admissible neighbors/decl/child boundary)",
                        file)
              .str();
      return plan;
    } else {
      // DELETE / REPLACE: intersect the patch's non-empty A-token range with
      // the PP-token range owned by this header/declaration. Only the
      // intersection can be materialized as a local header edit.
      uint64_t aLo = std::max(materialAStart, ppLo);
      uint64_t aHi = std::min(materialAEnd, ppHi);
      if (aHi <= aLo) {
        // Nothing of this patch lies in this header/declaration.
        continue;
      }

      // Find the first and last PP tokens in the intersected patch range that
      // actually map back to this header file. These tokens define the concrete
      // source byte range to replace/delete in the header.
      std::optional<uint64_t> firstPP, lastPP;
      for (uint64_t pp = aLo; pp < aHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && paths_.PathsEqual(it->second.file, file)) {
          if (!firstPP)
            firstPP = pp;
          lastPP = pp;
        }
      }

      if (!firstPP || !lastPP) {
        // The token intersection exists in PP space, but none of those tokens
        // have a source mapping into this header. Fall back to include
        // realization rather than manufacturing a header byte range.
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: no mapped PP tokens for patch[{0}] "
                          "in header file {1}",
                          idx, file)
                .str();
        return plan;
      }

      // Convert the mapped PP-token endpoints to a closed source byte envelope
      // in the header. The witness records both the PP-token evidence and the
      // resolved byte range so proof discharge can validate this include edit.
      startByte = ByteStartForPPInFile(file, *firstPP);
      endByte =
          ByteEndForPPInFile(file, *lastPP);
      mappedHeaderWitness.evidence =
          IncludeAnchorEvidenceKind::MappedHeaderTokens;
      mappedHeaderWitness.hasFirstPP = firstPP.has_value();
      mappedHeaderWitness.firstPP = firstPP ? *firstPP : 0ULL;
      mappedHeaderWitness.hasLastPP = lastPP.has_value();
      mappedHeaderWitness.lastPP = lastPP ? *lastPP : 0ULL;
      mappedHeaderWitness.hasByteRange =
          startByte.has_value() && endByte.has_value();
      mappedHeaderWitness.startByte = startByte ? *startByte : 0ULL;
      mappedHeaderWitness.endByte = endByte ? *endByte : 0ULL;

      // If either endpoint cannot be projected into header bytes, this patch
      // cannot be represented as an include-preserving mapped-header edit.
      if (!startByte || !endByte) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: failed to map first/last PP tokens "
                          "to bytes in file {0}",
                          file)
                .str();
        return plan;
      }

      // A single PP hunk can legitimately cross neighboring header-owned
      // source pieces when the bytes between the material tokens are complete
      // zero-token preprocessor structure. The ordinary mapped-token range is
      // intentionally local, but leaving the rest of the hunk in the header can
      // preserve stale source (`int`, a comma, or an include directive). Build
      // a full source envelope from header tokens and complete child-include
      // sites, then prove every inter-piece gap before overriding the local
      // range.
      const bool declClippedNonEmptyPatch =
          decl && (materialAStart < ppLo || ppHi < materialAEnd);
      struct HeaderSourceEnvelopePiece {
        uint64_t begin = 0;
        uint64_t end = 0;
        uint64_t ppBegin = 0;
        uint64_t ppEnd = 0;
        uint64_t id = 0;
        StringRef kind;
      };

      SmallVector<HeaderSourceEnvelopePiece, 8> hunkSourcePieces;
      bool hasCompleteChildIncludePiece = false;
      bool hasPartialChildIncludeOverlap = false;
      const uint64_t fullLo = std::max(materialAStart, coverBegin);
      const uint64_t fullHi = std::min(materialAEnd, coverEnd);

      for (uint64_t pp = fullLo; pp < fullHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it == tokmapByPP.end() || !paths_.PathsEqual(it->second.file, file))
          continue;

        // Require exact token byte bounds here: widening across header source
        // pieces needs precise endpoints on both sides of each gap.
        std::optional<uint64_t> tokBegin = ByteStartForPPInFile(file, pp);
        std::optional<uint64_t> tokEnd = ByteEndForPPInFile(file, pp);
        if (!tokBegin || !tokEnd || *tokBegin >= *tokEnd) {
          hunkSourcePieces.clear();
          break;
        }
        hunkSourcePieces.push_back(
            {*tokBegin, *tokEnd, pp, pp + 1, pp, "token"});
      }

      for (const auto &child : model_.GetIncludes()) {
        if (!child.parent || *child.parent != ie.include->id ||
            !paths_.PathsEqual(child.sitePath, file) || !child.cover.IsValid())
          continue;
        if (child.cover.end <= fullLo || fullHi <= child.cover.begin)
          continue;

        if (fullLo <= child.cover.begin && child.cover.end <= fullHi &&
            child.siteB < child.siteE && child.siteE <= headerText.size()) {
          hasCompleteChildIncludePiece = true;
          hunkSourcePieces.push_back({child.siteB, child.siteE,
                                      child.cover.begin, child.cover.end,
                                      child.id, "child-include"});
          continue;
        }

        // A partial child-include overlap would require editing inside the
        // child include and its parent directive at the same time. Leave that
        // to include realization rather than inventing a parent-local range.
        hasPartialChildIncludeOverlap = true;
      }

      bool hasCompleteMacroInvocationPiece = false;
      bool hasCompleteConditionalGroupPiece = false;
      bool hasPartialMacroInvocationOverlap = false;
      bool hasPartialConditionalGroupOverlap = false;

      for (const auto &macro : model_.GetMacroInvocations()) {
        if (!headerMacroInvocationOverlapsMaterial(macro, fullLo, fullHi))
          continue;
        if (headerMacroInvocationIsConsumedSourceEnvelope(macro, fullLo,
                                                          fullHi)) {
          hasCompleteMacroInvocationPiece = true;
          hunkSourcePieces.push_back({*macro.invB, *macro.invE,
                                      macro.cover.begin, macro.cover.end,
                                      macro.id, "macro-invocation"});
          continue;
        }

        // A partial source-bearing macro overlap means the parent header edit
        // would delete only part of the callsite's produced material.  That is
        // not a source-envelope proof; require a stronger/coalesced candidate.
        hasPartialMacroInvocationOverlap = true;
      }

      for (const auto &group : model_.GetConds()) {
        if (!headerConditionalGroupSelectedMaterialOverlaps(group, fullLo,
                                                            fullHi))
          continue;
        if (headerConditionalGroupIsConsumedSourceEnvelope(group, fullLo,
                                                           fullHi)) {
          hasCompleteConditionalGroupPiece = true;
          hunkSourcePieces.push_back({group.groupB, group.groupE, fullLo,
                                      fullHi, group.id, "conditional-group"});
          continue;
        }

        // The hunk touches selected conditional output but does not consume the
        // selected arm.  Do not widen across the directive wrapper unless a
        // coalesced patch can prove the whole group is replaced.
        hasPartialConditionalGroupOverlap = true;
      }

      const bool shouldTryFullHeaderEnvelope =
          declClippedNonEmptyPatch || hasCompleteChildIncludePiece ||
          hasCompleteMacroInvocationPiece || hasCompleteConditionalGroupPiece;
      const bool fullHeaderEnvelopeBlockedByPartialOverlap =
          shouldTryFullHeaderEnvelope &&
          (hasPartialChildIncludeOverlap || hasPartialMacroInvocationOverlap ||
           hasPartialConditionalGroupOverlap);
      const bool mustUseFullHeaderEnvelope =
          hasCompleteChildIncludePiece || hasCompleteMacroInvocationPiece ||
          hasCompleteConditionalGroupPiece ||
          fullHeaderEnvelopeBlockedByPartialOverlap;
      if (shouldTryFullHeaderEnvelope &&
          !fullHeaderEnvelopeBlockedByPartialOverlap) {
        std::optional<SourceEnvelopeProof> sourceEnvelope;
        if (hunkSourcePieces.size() >= 2 &&
            normalizeSourceEnvelopePieces(
                hunkSourcePieces,
                [](const HeaderSourceEnvelopePiece &lhs,
                   const HeaderSourceEnvelopePiece &rhs) {
                  return lhs.kind < rhs.kind;
                },
                [](const HeaderSourceEnvelopePiece &outer,
                   const HeaderSourceEnvelopePiece &piece) {
                  if (outer.kind == "child-include" && piece.kind == "token") {
                    // Tokens produced by a child include are owned by the child
                    // site. Once the complete child include is a
                    // source-envelope piece, any mapped child token inside that
                    // site is not a second parent piece.
                    return true;
                  }
                  if (outer.kind == "macro-invocation" &&
                      piece.kind == "token") {
                    // Macro-expanded tokens often map to the macro callsite
                    // spelling. Once the complete source-bearing invocation is
                    // proved consumed, those mapped token intervals are
                    // internal evidence, not separate parent-header pieces.
                    return true;
                  }
                  if (outer.kind == "conditional-group" &&
                      (piece.kind == "token" ||
                       piece.kind == "macro-invocation" ||
                       piece.kind == "child-include")) {
                    // A consumed conditional group owns the selected-arm source
                    // inside its directive wrapper. Nested token/macro/include
                    // pieces inside the group are covered by the group-level
                    // source-envelope proof.
                    return true;
                  }
                  return false;
                }))
          sourceEnvelope = {sourceEnvelopePieceBegin(hunkSourcePieces.front()),
                            sourceEnvelopePieceEnd(hunkSourcePieces.back())};

        SmallVector<HeaderPreservedGapPiece, 4> preservedPieces;
        if (sourceEnvelope) {
          for (const HeaderSourceEnvelopePiece &piece : hunkSourcePieces) {
            if (piece.kind != "child-include")
              continue;
            const RefoldModel::IncludeItem *child =
                model_.GetIncludeById(piece.id);
            if (!child)
              continue;
            if (!collectMacroStatePreservationsFromIncludeSubtree(
                    *child, StringRef(materialInsertBytes), preservedPieces)) {
              sourceEnvelope.reset();
              break;
            }
          }
        }
        if (sourceEnvelope) {
          auto pathIdentityPathsEqual = [this](StringRef lhs, StringRef rhs) {
            return paths_.PathsEqual(lhs, rhs);
          };

          SourceLineDirectiveLogicalLineRewriter
              headerSourceLineDirectiveLineRewriter =
                  [&](StringRef logicalLine, ArrayRef<uint64_t> sourceOffsets,
                      SourceLineDirectiveBuiltinMacroResolver
                          builtinMacroResolver)
              -> std::optional<SourceLineDirectiveLogicalLineRewrite> {
            return rewriteSourceLineDirectiveLogicalLineMacros(
                model_, file, logicalLine, sourceOffsets,
                pathIdentityPathsEqual, lexLang_, ie.include->id,
                std::move(builtinMacroResolver));
          };

          if (!proveSourceEnvelopeGaps(hunkSourcePieces, [&](uint64_t
                                                                      gapBegin,
                                                                  uint64_t
                                                                      gapEnd) {
                if (std::optional<SourceLineDirectiveGapResume> lineResume =
                        computeSourceLineDirectiveGapResume(
                            StringRef(headerText), gapBegin, gapEnd,
                            sourceEnvelope->end, file,
                            headerSourceLineDirectiveLineRewriter, nullptr,
                            model_.GetSourcePath(),
                            !sourceSuffixMayObservePresumedFileSpelling(
                                model_, file, sourceEnvelope->end,
                                pathIdentityPathsEqual, StringRef(headerText)))) {
                  // Header full-envelope widening uses the same owner-piece gap
                  // proof as TU/include closure.  A source-spelled
                  // line-control gap is not disposable trivia, but it is
                  // preservable by carrying its net line state forward to the
                  // untouched header suffix.
                  headerSourceLineDirectiveResume = std::move(lineResume);
                  return true;
                }

                SmallVector<HeaderPreservedGapPiece, 4> gapPieces;
                for (const auto &directive : model_.GetMacroDirectives()) {
                  std::optional<HeaderMacroStateDirectivePiece> directivePiece =
                      headerMacroStateDirectiveInterval(directive);
                  if (!directivePiece || directivePiece->begin < gapBegin ||
                      gapEnd < directivePiece->end)
                    continue;
                  if (macroStateProof_.ReplacementObservesMacroStateDirective(
                          *directivePiece->directive,
                          StringRef(materialInsertBytes),
                          /*unparseableObserves=*/true))
                    return false;

                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::MacroStateDirective;
                  piece.directive = directivePiece->directive;
                  piece.begin = directivePiece->begin;
                  piece.end = directivePiece->end;
                  piece.id = directivePiece->directive
                                 ? directivePiece->directive->id
                                 : 0;
                  gapPieces.push_back(std::move(piece));
                }

                for (const auto &group : model_.GetConds()) {
                  if (!headerConditionalGroupIsPreservableGap(group, gapBegin,
                                                              gapEnd))
                    continue;
                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::ZeroTokenConditionalGroup;
                  piece.begin = group.groupB;
                  piece.end = group.groupE;
                  piece.id = group.id;
                  gapPieces.push_back(std::move(piece));
                }

                // Pragma/state proof for materialized header owners.
                // A balanced diagnostic pragma island is a source-state atom:
                // the island may be preserved across a header source gap only
                // when push/pop depth returns to zero and every byte crossed by
                // the island is trivia.  This mirrors the TU source-envelope proof while
                // keeping non-diagnostic or unbalanced pragmas fail-closed.
                SmallVector<BalancedDiagnosticPragmaStateIsland, 4>
                    pragmaIslands;
                collectBalancedDiagnosticPragmaStateIslands(
                    model_, StringRef(headerText), gapBegin, gapEnd,
                    [&](const RefoldModel::PragmaDirective &pragma) {
                      return paths_.PathsEqual(pragma.sitePath, file);
                    },
                    pragmaIslands, lexLang_);
                for (const BalancedDiagnosticPragmaStateIsland &island :
                     pragmaIslands) {
                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::BalancedPragmaStateIsland;
                  piece.begin = island.begin;
                  piece.end = island.end;
                  piece.id = island.id;
                  gapPieces.push_back(std::move(piece));
                }

                for (const auto &macro : model_.GetMacroInvocations()) {
                  if (!headerMacroInvocationIsPreservableGap(macro, gapBegin,
                                                             gapEnd))
                    continue;
                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::ZeroTokenMacroInvocation;
                  piece.begin = *macro.invB;
                  piece.end = *macro.invE;
                  piece.id = macro.id;
                  gapPieces.push_back(std::move(piece));
                }

                for (const auto &child : model_.GetIncludes()) {
                  if (!headerZeroTokenChildIncludeIsPreservableGap(
                          child, gapBegin, gapEnd,
                          StringRef(materialInsertBytes)))
                    continue;
                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::ZeroTokenChildInclude;
                  piece.begin = child.siteB;
                  piece.end = child.siteE;
                  piece.id = child.id;
                  gapPieces.push_back(std::move(piece));
                }

                if (!proveSourceEnvelopeGap(
                        gapPieces, gapBegin, gapEnd,
                        [](const HeaderPreservedGapPiece &lhs,
                           const HeaderPreservedGapPiece &rhs) {
                          return static_cast<unsigned>(lhs.kind) <
                                 static_cast<unsigned>(rhs.kind);
                        },
                        [](const HeaderPreservedGapPiece &outer,
                           const HeaderPreservedGapPiece &piece) {
                          if (outer.kind == HeaderPreservedGapPiece::Kind::
                                                ZeroTokenMacroInvocation &&
                              piece.kind == HeaderPreservedGapPiece::Kind::
                                                ZeroTokenMacroInvocation) {
                            // Nested zero-token macro invocations are already
                            // part of the outer invocation's source-neutral
                            // proof. Preserve only the outer callsite text so
                            // the refolded header does not duplicate the same
                            // gap structure.
                            return true;
                          }

                          if (outer.kind == HeaderPreservedGapPiece::Kind::
                                                ZeroTokenConditionalGroup) {
                            // A preserved complete conditional group owns every
                            // artifact inside its source island after the
                            // shared conditional-island proof has discharged
                            // those artifacts. This includes nested conditional
                            // records, zero-token macro calls, zero-token or
                            // state-preserving child includes, direct
                            // macro-state directives in selected arms, and
                            // balanced pragma-state islands.
                            //
                            // Without this ownership rule, the gap tiler would
                            // see both the outer #if/#endif island and its
                            // already-proved child include/directive/pragma
                            // pieces as overlapping top-level proof pieces,
                            // reject the header envelope, and fall back to raw
                            // B.
                            return true;
                          }

                          return false;
                        },
                        [&](uint64_t begin, uint64_t end) {
                          return isWsOrCompleteCommentTrivia(
                              StringRef(headerText).slice(begin, end));
                        },
                        [](const HeaderPreservedGapPiece &) {}))
                  return false;

                preservedPieces.append(gapPieces.begin(), gapPieces.end());
                return true;
              }))
            sourceEnvelope.reset();
        }

        if (sourceEnvelope) {
          const bool hasHeaderSuffixAfterSourceLineDirective =
              headerSourceLineDirectiveResume &&
              sourceEnvelope->end < headerText.size();
          const bool canRestoreSourceLineState =
              !headerSourceLineDirectiveResume ||
              (!hasHeaderSuffixAfterSourceLineDirective ||
               (lineDirs_.Enabled() &&
                headerLineDirectiveStartsAtPrefix(sourceEnvelope->end) &&
                (!isDelete ||
                 canStartHeaderLineDirectiveWithOptionalLeadingNewline(
                     sourceEnvelope->begin))));

          if (sourceEnvelope->begin <= *startByte &&
              *endByte <= sourceEnvelope->end && canRestoreSourceLineState &&
              replacementHeaderSuffixBoundaryAllowsDirectiveLine(
                  StringRef(materialInsertBytes), sourceEnvelope->end)) {
            startByte = sourceEnvelope->begin;
            endByte = sourceEnvelope->end;
            headerGapPreservations = std::move(preservedPieces);
            usedFullHeaderEnvelope = true;
            mappedHeaderWitness.startByte = sourceEnvelope->begin;
            mappedHeaderWitness.endByte = sourceEnvelope->end;
            mappedHeaderWitness.firstPP = hunkSourcePieces.front().ppBegin;
            mappedHeaderWitness.lastPP = hunkSourcePieces.back().ppEnd - 1;
          }
        }
      }

      // If the replacement material intersects complete child source structure
      // owned by this header, a plain mapped-token edit is not a valid proof:
      // it can replace only the directly mapped parent tokens and leave stale
      // neighboring macro/include syntax behind.  Either the edit widened to
      // the proven source envelope above, or this include must be realized from
      // B.
      if (fullHeaderEnvelopeBlockedByPartialOverlap) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: patch[{0}] in header file {1} "
                          "partially overlaps source-envelope structure",
                          idx, file)
                .str();
        return plan;
      }
      if (mustUseFullHeaderEnvelope && !usedFullHeaderEnvelope) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: patch[{0}] in header file {1} "
                          "requires a full source-envelope proof",
                          idx, file)
                .str();
        return plan;
      }
    }

    // For DELETE/REPLACE, keep the resolved byte range inside the declaration
    // that owns the patch. INSERTs are different: their anchor may legitimately
    // sit on the boundary between declarations, so do not clamp insertion
    // anchors into one side of that boundary.
    if (!isInsert && decl && !usedFullHeaderEnvelope) {
      startByte = std::clamp(*startByte, decl->headerB, decl->headerE);
      endByte = std::clamp(*endByte, decl->headerB, decl->headerE);
    }

    // If no byte anchor/range was resolved, this patch cannot produce a local
    // header edit.
    if (!startByte)
      continue;

    // Clamp the final edit range to the physical header buffer. `endByte` is
    // clamped relative to `startByte` so malformed or boundary-adjusted ranges
    // collapse to a safe empty edit instead of inverting.
    const uint64_t fLen = static_cast<uint64_t>(fileLen);
    startByte = std::clamp(*startByte, uint64_t(0), fLen);
    endByte = std::clamp(*endByte, *startByte, fLen);

    // DELETE emits an empty replacement; INSERT and REPLACE both use the
    // B-derived patch payload carried by the IncludePatch.
    std::string replacement = isDelete ? "" : materialInsertBytes;

    if (isReplace && startByte && endByte && *startByte <= *endByte &&
        !replacement.empty() &&
        replacementHeaderSuffixBoundaryAllowsDirectiveLine(
            StringRef(replacement), *endByte)) {
      struct HeaderMacroStateCarryCandidate {
        const RefoldModel::MacroDirective *directive = nullptr;
        HeaderMacroStateDirectivePiece piece;
      };

      // Collect active header-owned #define directives that currently precede
      // this header-local replacement and that the B-derived replacement would
      // observe if they stayed there.  Those are exactly the definitions whose
      // source-order position must be reconsidered to make the replacement see
      // B's macro state.
      SmallVector<HeaderMacroStateCarryCandidate, 4> carryCandidates;
      for (const auto &directive : model_.GetMacroDirectives()) {
        if (directive.subkind != "#define")
          continue;

        std::optional<HeaderMacroStateDirectivePiece> piece =
            headerMacroStateDirectiveInterval(directive);
        if (!piece || piece->end > *startByte)
          continue;
        if (headerRangeOverlapsStagedEdit(piece->begin, piece->end))
          continue;
        if (!activeHeaderDefinitionAtByte(directive, piece->name, *startByte))
          continue;
        if (!macroStateProof_.ReplacementObservesMacroStateDirective(
                directive, StringRef(replacement),
                /*unparseableObserves=*/false))
          continue;

        // Header materialization has the same placement-sensitive macro-state
        // invariant as TU gap carry.  If an active #define remains before a
        // header-local replacement, the replacement is preprocessed under the
        // old macro state rather than B's macro state.  We may carry that
        // definition after the replacement only when the crossed original
        // header bytes do not themselves observe the definition; otherwise
        // those bytes are part of the old zero-token proof and must keep the
        // original source order.
        if (macroStateProof_.SourceChunkObservesMacroStateDirectiveWhenCrossed(
                directive, piece->name,
                StringRef(headerText).slice(piece->end, *startByte),
                StringRef(headerText).slice(*startByte, *endByte)))
          continue;

        carryCandidates.push_back(
            HeaderMacroStateCarryCandidate{&directive, *piece});
      }

      if (!carryCandidates.empty()) {
        // Apply carries in source order so widening the replacement is
        // deterministic and so each directive is removed from its original
        // position exactly once before being re-emitted after the payload.
        llvm::sort(carryCandidates,
                   [](const HeaderMacroStateCarryCandidate &lhs,
                      const HeaderMacroStateCarryCandidate &rhs) {
                     if (lhs.piece.begin != rhs.piece.begin)
                       return lhs.piece.begin < rhs.piece.begin;
                     return lhs.directive->id < rhs.directive->id;
                   });

        bool admissibleCarry = true;
        const uint64_t newStart = carryCandidates.front().piece.begin;
        uint64_t cursor = newStart;
        for (const HeaderMacroStateCarryCandidate &candidate :
             carryCandidates) {
          if (cursor > candidate.piece.begin) {
            admissibleCarry = false;
            break;
          }
          StringRef crossed =
              StringRef(headerText).slice(cursor, candidate.piece.begin);
          StringRef following =
              StringRef(headerText).slice(candidate.piece.begin, *startByte);
          // Once an earlier definition is carried, every byte crossed before
          // the next carried directive must remain non-observing for that prior
          // definition.  This keeps multi-directive carries from silently
          // moving a macro past source that depended on the old ordering.
          for (const HeaderMacroStateCarryCandidate &prior : carryCandidates) {
            if (prior.piece.end <= cursor)
              continue;
            if (prior.piece.begin >= candidate.piece.begin)
              break;
            if (macroStateProof_.SourceChunkObservesMacroStateDirectiveWhenCrossed(
                    *prior.directive, prior.piece.name, crossed, following)) {
              admissibleCarry = false;
              break;
            }
          }
          if (!admissibleCarry)
            break;
          cursor = candidate.piece.end;
        }

        if (admissibleCarry) {
          uint64_t replacementTailEnd = *endByte;
          if (replacementTailEnd < headerText.size() &&
              !stringutils::isBOL(StringRef(headerText),
                                  static_cast<size_t>(replacementTailEnd))) {
            size_t nl = StringRef(headerText).find('\n', replacementTailEnd);
            replacementTailEnd = nl == StringRef::npos
                                     ? static_cast<uint64_t>(headerText.size())
                                     : static_cast<uint64_t>(nl + 1);
          }

          // A carried directive must be re-emitted at a physical line boundary.
          // If the replacement stops in the middle of an original source line,
          // appending the directive immediately would split the surviving line
          // (`USE(3) #define ... ;`).  Carry the non-observing tail of that
          // physical line into the widened edit before re-emitting directives.
          // This also consumes a single original newline when the replacement
          // already ends at the previous line's last token, avoiding a spurious
          // blank line before the untouched suffix.
          if (replacementTailEnd > *endByte) {
            StringRef tail =
                StringRef(headerText).slice(*endByte, replacementTailEnd);
            StringRef following =
                StringRef(headerText).drop_front(replacementTailEnd);
            for (const HeaderMacroStateCarryCandidate &candidate :
                 carryCandidates) {
              if (macroStateProof_.SourceChunkObservesMacroStateDirectiveWhenCrossed(
                      *candidate.directive, candidate.piece.name, tail,
                      following)) {
                admissibleCarry = false;
                break;
              }
            }
          }

          std::string carriedReplacement;
          carriedReplacement.reserve((*startByte - newStart) +
                                     replacement.size() +
                                     (replacementTailEnd - *endByte) + 64);

          // Rebuild the widened header edit as:
          //   preserved bytes before carried directives,
          //   B-derived replacement payload,
          //   non-observing physical line tail after the payload,
          //   carried #define directive lines.
          // The directive bytes are intentionally omitted from their original
          // locations and appended after the completed payload line so the
          // payload is not macro-expanded by definitions that B did not have
          // active there, while the surviving suffix still sees the restored
          // macro state.
          cursor = newStart;
          for (const HeaderMacroStateCarryCandidate &candidate :
               carryCandidates) {
            if (cursor > candidate.piece.begin) {
              admissibleCarry = false;
              break;
            }
            carriedReplacement.append(headerText.begin() + cursor,
                                      headerText.begin() +
                                          candidate.piece.begin);
            cursor = candidate.piece.end;
          }

          if (admissibleCarry) {
            carriedReplacement.append(headerText.begin() + cursor,
                                      headerText.begin() + *startByte);
            carriedReplacement += replacement;
            if (replacementTailEnd > *endByte) {
              carriedReplacement.append(headerText.begin() + *endByte,
                                        headerText.begin() +
                                            replacementTailEnd);
            } else if (!carriedReplacement.empty() &&
                       carriedReplacement.back() != '\n') {
              carriedReplacement.push_back('\n');
            }
            for (const HeaderMacroStateCarryCandidate &candidate :
                 carryCandidates) {
              carriedReplacement.append(candidate.directive->text.begin(),
                                        candidate.directive->text.end());
              if (carriedReplacement.empty() ||
                  carriedReplacement.back() != '\n')
                carriedReplacement.push_back('\n');
            }

            *startByte = newStart;
            *endByte = replacementTailEnd;
            replacement = std::move(carriedReplacement);
            mappedHeaderWitness.hasByteRange = true;
            mappedHeaderWitness.startByte = *startByte;
            mappedHeaderWitness.endByte = *endByte;
          }
        }
      }
    }

    if (!headerGapPreservations.empty()) {
      // Emit preserved gap pieces after the B-derived payload. That keeps
      // replacement tokens in the same preprocessing environment as B, while
      // the untouched header suffix sees the restored zero-token structure.
      //
      // Balanced diagnostic pragma islands are the one side-effect gap class
      // that may already be present in the B-derived replay bytes.  Sideband
      // normalization removes their directive tokens from the structural diff,
      // but the raw replacement slice still contains the emitted `#pragma`
      // lines.  When that replay surface canonically carries the same balanced
      // island, suppress the source-gap copy so the pragma state is represented
      // exactly once.
      bool insertedSeparator = false;
      for (const HeaderPreservedGapPiece &piece : headerGapPreservations) {
        std::string pieceText = preservedGapPieceText(piece);
        if (pieceText.empty())
          continue;

        if (piece.kind ==
                HeaderPreservedGapPiece::Kind::BalancedPragmaStateIsland &&
            balancedDiagnosticPragmaStateIslandIsCarriedByReplacement(
                StringRef(pieceText), StringRef(replacement), lexLang_)) {
          continue;
        }

        if (!insertedSeparator && !replacement.empty() &&
            replacement.back() != '\n') {
          replacement.push_back('\n');
          insertedSeparator = true;
        }
        replacement += pieceText;
        if (replacement.empty() || replacement.back() != '\n')
          replacement.push_back('\n');
      }
    }

    const bool emitsHeaderSourceLineDirectiveResume =
        lineDirs_.Enabled() && usedFullHeaderEnvelope &&
        headerSourceLineDirectiveResume && *endByte < fileLen;
    std::vector<FinalLineControlPruneCandidate> sourceLineResumeCandidates;
    if (emitsHeaderSourceLineDirectiveResume) {
      // A full-envelope delete has no B-derived payload, but the consumed
      // source interval may still have carried a line-control directive whose
      // net state must be restored before the untouched header suffix. Emit a
      // resync-only replacement in that case; non-empty replacements get the
      // same directive on a fresh physical line after their edited payload.
      if (replacement.empty()) {
        if (!headerLineDirectiveStartsAtPrefix(*startByte))
          replacement.push_back('\n');
      } else if (replacement.back() != '\n') {
        replacement.push_back('\n');
      }
      const uint64_t resumeBegin = static_cast<uint64_t>(replacement.size());
      replacement +=
          formatSourceLineDirectiveGapResume(*headerSourceLineDirectiveResume);
      const uint64_t resumeEnd = static_cast<uint64_t>(replacement.size());
      sourceLineResumeCandidates.push_back(
          makeSyntheticLineControlPruneCandidate(
              resumeBegin, resumeEnd,
              FinalLineDirective::Origin::SyntheticSourceLineResume,
              FinalLineControlOwnerKey(file, ie.include->id),
              FinalLineControlObligation::HeaderResumeRepair));

      const OwnerStateBoundary sourceLineResumeBoundary =
          OwnerStateBoundary::FromSource(OwnerSourceRange::From(
              file, *endByte, *endByte,
              std::optional<uint64_t>(ie.include->id)));
      auto checkHeaderSourceLineResume = [&](OwnerStateComponent component) {
        const std::string detail =
            llvm::formatv("synthetic header source #line resume for include "
                          "#{0} patch[{1}] component={2} sourceEnd={3}",
                          ie.include->id, idx, component, *endByte)
                .str();
        (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
            sourceLineResumeBoundary, component, StateMutationKind::Replayed,
            ownerStateProof_.BuildStateTransitionWitness(SuffixStabilityWitnessKind::StateRepair,
                                        component, sourceLineResumeBoundary,
                                        detail),
            "include/source-line-resume", detail,
            /*requireKnownObserver=*/false);
      };
      checkHeaderSourceLineResume(OwnerStateComponent::LineNumber);
      checkHeaderSourceLineResume(OwnerStateComponent::FileState);
      checkHeaderSourceLineResume(OwnerStateComponent::FileName);

    }

    // Queue the include-preserving mapped-header edit and attach the witness
    // that records which PP tokens and header bytes justified the edit range.
    TextEdit edit = emitsHeaderSourceLineDirectiveResume
                        ? TextEdit{*startByte,
                                   *endByte,
                                   std::move(replacement),
                                   std::nullopt,
                                   std::nullopt,
                                   {},
                                   {},
                                   {}}
                        : MakeTextEditWithResyncOrPending(
                              headerText, *startByte, *endByte, replacement,
                              file, ie.include->id);
    if (emitsHeaderSourceLineDirectiveResume)
      edit.lineControlPruneCandidates = std::move(sourceLineResumeCandidates);
    textEditAssembler_.AttachAcceptedResultCarrier(
        edit, proofLattice_.BuildAcceptedIncludeCandidate(
                  AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens, p,
                  &mappedHeaderWitness));
    plan.edits.push_back(std::move(edit));
  }

  // Apply all edits inside this header, highest offset first so earlier edits
  // do not disturb the coordinates of later ones.
  sort(plan.edits, [](const TextEdit &lhs, const TextEdit &rhs) {
    if (lhs.start != rhs.start)
      return lhs.start > rhs.start;
    return lhs.end > rhs.end;
  });

  for (const auto &e : plan.edits) {

    if (e.end < e.start || e.end > static_cast<uint64_t>(headerText.size())) {
      REFOLD_LOG_FATAL("include/apply", "TextEdit out of bounds: bytes=[{0},{1}) size={2}",
            e.start, e.end, headerText.size());
    }
  }

  return plan;
}

std::optional<uint64_t> RefoldIncludeMaterializer::ComputeChildBoundaryInsertByte(
    const IncludePatch &p, StringRef file,
    IncludeAnchorWitness *witness) const {
  // This is the declared IncludeInsertionByChildBoundary proof. It is
  // intentionally narrow: a parent-owned pure insertion may anchor at a
  // preserved direct child include only when the PP gap equals exactly one
  // child cover boundary. Anything inside a child cover belongs to the child
  // owner, and any ambiguous sibling boundary fails closed instead of guessing.
  //
  // Child-boundary anchoring only applies to patches owned by a concrete parent
  // include. Without that owner, there is no child include set to inspect.
  const RefoldModel::IncludeItem *owner = p.include;
  if (!owner)
    return std::nullopt;

  // The INSERT hunk's A-side start is the PP-gap where the new text should be
  // anchored. This helper only recognizes gaps that coincide exactly with a
  // direct child include's cover boundary.
  uint64_t pos = p.aStart;

  SmallVector<const RefoldModel::IncludeItem *, 4> beginMatches;
  SmallVector<const RefoldModel::IncludeItem *, 4> endMatches;

  for (const auto &child : model_.GetIncludes()) {
    // Only direct children of this include can supply a boundary in the current
    // parent header.
    if (!child.parent || *child.parent != owner->id)
      continue;

    // The child include directive must be spelled in the header file currently
    // being materialized.
    if (!paths_.PathsEqual(child.sitePath, file))
      continue;

    const uint64_t cb = child.cover.begin;
    const uint64_t ce = child.cover.end;

    // A gap strictly inside the child's PP cover is not a parent boundary. That
    // edit should be represented as child-owned work, not as an insertion at
    // the parent include site.
    if (cb < pos && pos < ce)
      return std::nullopt;

    if (cb == pos)
      beginMatches.push_back(&child);
    if (ce == pos)
      endMatches.push_back(&child);
  }

  auto dump = [](ArrayRef<const RefoldModel::IncludeItem *> v) -> std::string {
    std::string out;
    bool first = true;
    for (const auto *c : v) {
      if (!first)
        out += "; ";
      first = false;
      out +=
          formatv("child#{0} cover=[{1},{2}) site=[{3},{4}) target={5}", c->id,
                  c->cover.begin, c->cover.end, c->siteB, c->siteE, c->target)
              .str();
    }
    return out;
  };

  // Prefer anchoring before a child include whose PP cover begins at the gap.
  // The returned byte is the beginning of the child's spelled include
  // directive.
  if (!beginMatches.empty()) {
    if (beginMatches.size() != 1) {
      REFOLD_LOG_FATAL("include/boundary",
            "ambiguous child include boundary: {0} children have cover.begin "
            "== {1} in file={2}; candidates: {3}",
            beginMatches.size(), pos, file, dump(beginMatches));
    }
    if (witness) {
      witness->evidence = IncludeAnchorEvidenceKind::ChildBoundary;
      witness->hasChildIncludeId = true;
      witness->childIncludeId = beginMatches[0]->id;
      witness->hasAnchorByte = true;
      witness->anchorByte = beginMatches[0]->siteB;
    }
    return beginMatches[0]->siteB;
  }

  // Otherwise, anchor after the unique child include whose PP cover ends at the
  // gap. The returned byte is the end of the child's spelled include directive.
  if (!endMatches.empty()) {
    if (endMatches.size() != 1) {
      REFOLD_LOG_FATAL("include/boundary",
            "ambiguous child include boundary: {0} children have cover.end == "
            "{1} in file={2}; candidates: {3}",
            endMatches.size(), pos, file, dump(endMatches));
    }
    if (witness) {
      witness->evidence = IncludeAnchorEvidenceKind::ChildBoundary;
      witness->hasChildIncludeId = true;
      witness->childIncludeId = endMatches[0]->id;
      witness->hasAnchorByte = true;
      witness->anchorByte = endMatches[0]->siteE;
    }
    return endMatches[0]->siteE;
  }

  return std::nullopt;
}
} // namespace refold
} // namespace clang

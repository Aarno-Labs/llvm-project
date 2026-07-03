//===--- RefoldHeaderIncludeEditPlanner.cpp --------------------*- C++ -*-===//
//
// Header-local include edit planning for clang-refold.
//
// This file implements the detailed header edit planner extracted from
// RefoldIncludeMaterializer.  It preserves the existing deterministic edit
// planning order while moving source-envelope, macro-state, line-control, and
// insertion-anchor decisions behind a dedicated service boundary.
//
//===----------------------------------------------------------------------===//

#include "include/RefoldHeaderIncludeEditPlanner.h"
#include "core/RefoldLog.h"
#include "edit/RefoldSourceEnvelopeTiling.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/IncludeSpellingHelpers.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineControlEditHelpers.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
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

#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
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

/// Returns the byte width of a recorded header declaration span.
/// Malformed spans are treated as zero-width for deterministic tie-breaking.
static uint64_t headerDeclSpanLength(const RefoldModel::HeaderDecl *decl) {
  if (decl->span.end <= decl->span.begin)
    return 0;
  return decl->span.end - decl->span.begin;
}

/// Formats child include candidates for deterministic boundary diagnostics.
/// The caller uses this only on fatal ambiguity paths.
static std::string formatIncludeBoundaryCandidates(
    ArrayRef<const RefoldModel::IncludeItem *> candidates) {
  std::string out;
  bool first = true;
  for (const auto *child : candidates) {
    if (!first)
      out += "; ";
    first = false;
    out += formatv("child#{0} cover=[{1},{2}) site=[{3},{4}) target={5}",
                   child->id, child->cover.begin, child->cover.end,
                   child->siteB, child->siteE, child->target)
               .str();
  }
  return out;
}

/// Returns whether two token ranges have identical token spellings. Invalid
/// ranges or ranges of different lengths are treated as non-matching.
static bool tokenRangesHaveSameSpelling(ArrayRef<PPTok> aToks,
                                        ArrayRef<PPTok> bToks, uint64_t aBegin,
                                        uint64_t aEnd, uint64_t bBegin,
                                        uint64_t bEnd) {
  if (aEnd < aBegin || bEnd < bBegin || (aEnd - aBegin) != (bEnd - bBegin))
    return false;
  if (aEnd > static_cast<uint64_t>(aToks.size()) ||
      bEnd > static_cast<uint64_t>(bToks.size()))
    return false;
  for (uint64_t i = 0; i < aEnd - aBegin; ++i)
    if (aToks[static_cast<size_t>(aBegin + i)].spelling !=
        bToks[static_cast<size_t>(bBegin + i)].spelling)
      return false;
  return true;
}

/// Returns whether an owner-local byte gap contains source-authored #line
/// state.  This syntactic predicate guards boundary-token ties that would cross
/// a gap capable of changing the copied suffix's logical location.
static bool sourceIntervalContainsLineControlDirective(StringRef source,
                                                       uint64_t begin,
                                                       uint64_t end) {
  if (begin >= end || end > source.size())
    return false;

  uint64_t lineBegin = begin;
  while (lineBegin < end) {
    std::string logicalLine;
    uint64_t afterLine = lineBegin;
    if (!collectLineSpliceLogicalLine(source, lineBegin, end, logicalLine,
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
}

/// Returns whether the suffix at `anchorByte` begins with a preprocessor line.
/// Leading horizontal whitespace on that line is treated as part of the
/// directive boundary.
static bool suffixBeginsWithPreprocessorDirective(StringRef headerText,
                                                  uint64_t anchorByte) {
  size_t pos = static_cast<size_t>(anchorByte);
  if (pos >= headerText.size())
    return false;

  // Local line resync is still required before a preserved directive line.
  // Unlike ordinary suffix tokens, directives such as #include, #define,
  // #undef, and #line observe their presumed source location through
  // preprocessing state and diagnostics.  Horizontal indentation before the '#'
  // belongs to the directive line and is a valid flush point for the pending
  // #line.
  size_t lineEnd = headerText.find('\n', pos);
  if (lineEnd == StringRef::npos)
    lineEnd = headerText.size();
  while (pos < lineEnd && (headerText[pos] == ' ' || headerText[pos] == '\t'))
    ++pos;
  return pos < lineEnd && headerText[pos] == '#';
}

/// Returns whether replay text contains any preprocessor directive line.
/// Each physical line is checked after skipping leading spaces and tabs.
static bool replayContainsPreprocessorDirectiveLine(StringRef replay) {
  size_t lineBegin = 0;
  while (lineBegin < replay.size()) {
    size_t lineEnd = replay.find('\n', lineBegin);
    if (lineEnd == StringRef::npos)
      lineEnd = replay.size();

    size_t pos = lineBegin;
    while (pos < lineEnd && (replay[pos] == ' ' || replay[pos] == '\t'))
      ++pos;
    if (pos < lineEnd && replay[pos] == '#')
      return true;

    if (lineEnd == replay.size())
      break;
    lineBegin = lineEnd + 1;
  }
  return false;
}

} // namespace

RefoldHeaderIncludeEditPlanner::RefoldHeaderIncludeEditPlanner(
    const RefoldModel &model, StringRef aSource, StringRef bSource,
    ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff,
    const std::vector<int64_t> &abTokMapA2B,
    const LineDirectiveInserter &lineDirs,
    const RefoldSourceMapper &sourceMapper, const RefoldPathIdentity &paths,
    const RefoldMacroStateProof &macroStateProof,
    const RefoldLineControlProof &lineControlProof,
    const RefoldOwnerStateProof &ownerStateProof,
    const RefoldProofLattice &proofLattice,
    const RefoldTextEditAssembler &textEditAssembler,
    const RefoldTerminalProofSink &terminalSink,
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    const clang::LangOptions &lexLang)
    : model_(model), aSource_(aSource), bSource_(bSource), aToks_(aToks),
      bToks_(bToks), bTokOff_(bTokOff), abTokMapA2B_(abTokMapA2B),
      lineDirs_(lineDirs), sourceMapper_(sourceMapper), paths_(paths),
      macroStateProof_(macroStateProof), lineControlProof_(lineControlProof),
      ownerStateProof_(ownerStateProof), proofLattice_(proofLattice),
      textEditAssembler_(textEditAssembler),
      sidebandPragmaEdits_(sidebandPragmaEdits), lexLang_(lexLang) {
  // The terminal sink remains part of the construction API for parity with the
  // materializer service boundary, but this planner does not currently request
  // terminal fallback directly.
  (void)terminalSink;
}

RefoldHeaderIncludeEditPlanner::TextEdit
RefoldHeaderIncludeEditPlanner::MakeTextEditWithResyncOrPending(
    StringRef original, uint64_t start, uint64_t end, StringRef replacement,
    StringRef fileSpelling, std::optional<uint64_t> ownerIncludeId) const {
  ResyncOutcome outcome = textEditAssembler_.ApplyResyncOrPend(
      original, start, end, replacement, fileSpelling, ownerIncludeId);
  TextEdit edit{start,
                end,
                std::move(outcome.text),
                std::move(outcome.pending),
                std::nullopt,
                {},
                {}};
  edit.lineControlPruneCandidates =
      std::move(outcome.lineControlPruneCandidates);
  return edit;
}

const RefoldModel::HeaderDecl *
RefoldHeaderIncludeEditPlanner::FindHeaderDeclForPatch(
    const RefoldModel::IncludeItem &inc, const IncludePatch &p) {
  if (inc.decls.empty())
    return nullptr;

  const uint64_t aLo = p.aStart;
  const uint64_t aHi = p.aEnd;

  const RefoldModel::HeaderDecl *bestCover = nullptr;
  const RefoldModel::HeaderDecl *bestOverlap = nullptr;

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
        if (!bestCover || curSpanLen < headerDeclSpanLength(bestCover))
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
      if (!bestCover || curSpanLen < headerDeclSpanLength(bestCover))
        bestCover = &d;
    } else if (overlaps) {
      if (!bestOverlap || curSpanLen < headerDeclSpanLength(bestOverlap))
        bestOverlap = &d;
    }
  }

  return bestCover ? bestCover : bestOverlap;
}

struct RefoldHeaderIncludeEditPlanner::HeaderIncludeNeutralityAdapter {
  /// Calls the planner's named include-neutrality predicate for callbacks that
  /// require borrowed callable storage.
  bool operator()(const RefoldModel::IncludeItem &child) const {
    return planner.HeaderIncludeIsSourceNeutralZeroToken(currentInclude, file,
                                                         headerText, child);
  }

  /// Planner that owns the named include-neutrality predicate.
  const RefoldHeaderIncludeEditPlanner &planner;

  /// Include whose materialized header is currently being rewritten.
  const RefoldModel::IncludeItem &currentInclude;

  /// Entered-file spelling for the current materialized header.
  StringRef file;

  /// Current materialized header bytes.
  StringRef headerText;
};

bool RefoldHeaderIncludeEditPlanner::HeaderConditionalGroupIsPreservableGap(
    const HeaderSourceNeutralityContext &headerSourceNeutrality,
    const RefoldModel::IncludeItem &currentInclude, StringRef file,
    StringRef headerText, const RefoldModel::CondGroup &group,
    uint64_t gapBegin, uint64_t gapEnd) const {
  HeaderIncludeNeutralityAdapter includeIsNeutral{*this, currentInclude, file,
                                                  headerText};
  NeutralConditionalIslandContext islandContext{
      /*requireGroupBeginAtLineStart=*/true,
      NeutralConditionalArmSpanMode::SelectedArmsOnly};
  return RefoldSourceNeutralityProof::ConditionalGroupIsNeutralIsland(
      headerSourceNeutrality, group, gapBegin, gapEnd, islandContext,
      includeIsNeutral);
}

bool RefoldHeaderIncludeEditPlanner::
    HeaderMacroInvocationIsSourceNeutralZeroToken(
        const HeaderSourceNeutralityContext &headerSourceNeutrality,
        const RefoldModel::MacroInvocation &invocation) const {
  return RefoldSourceNeutralityProof::MacroInvocationIsSourceNeutralZeroToken(
      headerSourceNeutrality, invocation);
}

bool RefoldHeaderIncludeEditPlanner::HeaderMacroInvocationIsPreservableGap(
    const HeaderSourceNeutralityContext &headerSourceNeutrality,
    const RefoldModel::IncludeItem &currentInclude, StringRef file,
    StringRef headerText, const RefoldModel::MacroInvocation &macro,
    uint64_t gapBegin, uint64_t gapEnd) const {
  // The invocation must be physically spelled in this materialized header gap
  // and owned by the include instance currently being rewritten.
  if (!macro.invFile || macro.invFile->empty() ||
      !paths_.PathsEqual(*macro.invFile, file))
    return false;
  if (!macro.ownerIncludeId || *macro.ownerIncludeId != currentInclude.id)
    return false;
  if (!macro.invB || !macro.invE || *macro.invB >= *macro.invE)
    return false;
  if (*macro.invB < gapBegin || gapEnd < *macro.invE)
    return false;
  if (*macro.invE > headerText.size())
    return false;
  // Byte-verify the recorded callsite text before preserving it.  This keeps
  // stale or ambiguous refold-map offsets from authorizing a source copy.
  if (macro.invText &&
      headerText.slice(*macro.invB, *macro.invE) != *macro.invText)
    return false;

  return HeaderMacroInvocationIsSourceNeutralZeroToken(headerSourceNeutrality,
                                                       macro);
}

bool RefoldHeaderIncludeEditPlanner::HeaderIncludeIsDescendantOf(
    const RefoldModel::IncludeItem &candidate,
    const RefoldModel::IncludeItem &root) const {
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
}

bool RefoldHeaderIncludeEditPlanner::
    HeaderZeroTokenChildIncludeIsPreservableGap(
        const RefoldModel::IncludeItem &currentInclude, StringRef file,
        StringRef headerText, const RefoldModel::IncludeItem &child,
        uint64_t gapBegin, uint64_t gapEnd, StringRef replacement) const {
  // Preserve only complete direct child include directives spelled in the
  // current parent header gap.  Nested descendants are checked below as part of
  // proving that the entire child include subtree is token-neutral.
  if (!child.parent || *child.parent != currentInclude.id)
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
      headerText.slice(child.siteB, child.siteE) != child.text)
    return false;

  // A zero-token include subtree may contain nested include directives, but
  // none of those descendants may contribute materialized PP tokens.
  for (const auto &inc : model_.GetIncludes()) {
    if (&inc == &child)
      continue;
    if (!HeaderIncludeIsDescendantOf(inc, child))
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
            HeaderIncludeIsDescendantOf(inc, child)) {
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
    // selected arm produced PP material.  Otherwise preserving the include as a
    // zero-token gap would silently discard live conditional output.
    if (!group.parentIncludeId)
      continue;
    bool ownedByChild = *group.parentIncludeId == child.id;
    if (!ownedByChild)
      for (const auto &inc : model_.GetIncludes())
        if (inc.id == *group.parentIncludeId &&
            HeaderIncludeIsDescendantOf(inc, child)) {
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
    // Macro-state transitions in a preserved zero-token include become visible
    // after the replacement payload is emitted.  Reject the preservation if the
    // B-derived replacement text mentions that macro name, because then moving
    // the transition could change how the replacement preprocesses.
    if (!directive.ownerIncludeId)
      continue;
    bool ownedByChild = *directive.ownerIncludeId == child.id;
    if (!ownedByChild)
      for (const auto &inc : model_.GetIncludes())
        if (inc.id == *directive.ownerIncludeId &&
            HeaderIncludeIsDescendantOf(inc, child)) {
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
}

bool RefoldHeaderIncludeEditPlanner::HeaderIncludeIsSourceNeutralZeroToken(
    const RefoldModel::IncludeItem &currentInclude, StringRef file,
    StringRef headerText, const RefoldModel::IncludeItem &child) const {
  // Header conditional islands are preserved verbatim, so an include inside a
  // selected arm is admissible when the ordinary header include-gap proof can
  // prove the complete child include subtree contributes no PP material.  Use
  // the full materialized header as the gap surface here: the containing
  // conditional proof has already checked that the include site lies inside the
  // preserved group.
  return HeaderZeroTokenChildIncludeIsPreservableGap(
      currentInclude, file, headerText, child, 0, headerText.size(),
      StringRef());
}

bool RefoldHeaderIncludeEditPlanner::PPSpanInsideHeaderMaterial(
    const RefoldModel::PPSpan &span, uint64_t materialBeginA,
    uint64_t materialEndA) {
  if (!span.IsValid() || span.begin >= span.end)
    return true;
  return materialBeginA <= span.begin && span.end <= materialEndA;
}

bool RefoldHeaderIncludeEditPlanner::HeaderArmIsSameOrNestedUnder(
    const RefoldModel::ArmRef &candidate, uint64_t ancestorArmId) const {
  if (!candidate.group || !candidate.arm)
    return false;
  if (candidate.arm->id == ancestorArmId)
    return true;

  // FindArmRefAtPP() reports the innermost selected arm that owns a token. When
  // proving that an outer header conditional group is wholly consumed, tokens
  // in nested selected arms still count as effective material of the outer arm.
  // Walk parent-arm links until either the queried arm is found or the chain
  // leaves the nested conditional tree.
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
}

bool RefoldHeaderIncludeEditPlanner::HeaderSelectedArmEffectiveMaterialInside(
    const RefoldModel::CondArm &arm, uint64_t materialBeginA,
    uint64_t materialEndA) const {
  bool sawEffectiveMaterial = false;
  for (uint64_t pp = 0; pp < aToks_.size(); ++pp) {
    std::optional<RefoldModel::ArmRef> owner = model_.FindArmRefAtPP(pp);
    if (!owner || !HeaderArmIsSameOrNestedUnder(*owner, arm.id))
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
         PPSpanInsideHeaderMaterial(*arm.span, materialBeginA, materialEndA);
}

std::string RefoldHeaderIncludeEditPlanner::PreservedGapPieceText(
    const HeaderPreservedGapPiece &piece, StringRef headerText) {
  // Macro-state directives are preserved from the producer's recorded text so
  // spelling such as leading whitespace and line continuations matches the
  // directive record, not a best-effort slice reconstructed from ownership.
  if (piece.kind == HeaderPreservedGapPiece::Kind::MacroStateDirective &&
      piece.directive)
    return piece.directive->text.str();

  // Other preserved gap pieces are source intervals in the materialized header
  // buffer. Require a valid in-buffer range before copying bytes into the
  // replacement payload.
  if (piece.end <= headerText.size() && piece.begin <= piece.end)
    return headerText.slice(piece.begin, piece.end).str();
  return std::string();
}

bool RefoldHeaderIncludeEditPlanner::
    HeaderConditionalGroupIsConsumedSourceEnvelope(
        const RefoldModel::IncludeItem &currentInclude, StringRef file,
        StringRef headerText, const RefoldModel::CondGroup &group,
        uint64_t materialBeginA, uint64_t materialEndA) const {
  if (!paths_.PathsEqual(group.file, file))
    return false;
  if (!group.parentIncludeId || *group.parentIncludeId != currentInclude.id)
    return false;
  if (group.groupB >= group.groupE || group.groupE > headerText.size())
    return false;

  // This is the source-bearing counterpart of the zero-token conditional gap
  // proof. The complete directive group may become a source-envelope piece only
  // when the selected arm's effective PP material is wholly consumed by the
  // A-side replacement material. Otherwise copying/removing the group would
  // move surviving tokens across conditional-control structure.
  for (const RefoldModel::CondArm &arm : group.arms) {
    if (!arm.selected)
      continue;
    if (HeaderSelectedArmEffectiveMaterialInside(arm, materialBeginA,
                                                 materialEndA))
      return true;
  }
  return false;
}

bool RefoldHeaderIncludeEditPlanner::
    HeaderConditionalGroupSelectedMaterialOverlaps(
        const RefoldModel::IncludeItem &currentInclude, StringRef file,
        const RefoldModel::CondGroup &group, uint64_t materialBeginA,
        uint64_t materialEndA) const {
  if (!paths_.PathsEqual(group.file, file))
    return false;
  if (!group.parentIncludeId || *group.parentIncludeId != currentInclude.id)
    return false;
  for (const RefoldModel::CondArm &arm : group.arms) {
    if (!arm.selected || !arm.span || !arm.span->IsValid() ||
        arm.span->begin >= arm.span->end)
      continue;
    if (materialBeginA < arm.span->end && arm.span->begin < materialEndA)
      return true;
  }
  return false;
}

bool RefoldHeaderIncludeEditPlanner::IncludeOwnsDirective(
    const RefoldModel::MacroDirective &directive,
    const RefoldModel::IncludeItem &root) const {
  if (!directive.ownerIncludeId)
    return false;
  if (*directive.ownerIncludeId == root.id)
    return true;
  for (const auto &inc : model_.GetIncludes())
    if (inc.id == *directive.ownerIncludeId)
      return HeaderIncludeIsDescendantOf(inc, root);
  return false;
}

bool RefoldHeaderIncludeEditPlanner::RecordedMacroDirectiveMatchesOwnerFile(
    const RefoldModel::MacroDirective &directive) const {
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
  return macroStateProof_
      .RecoverMacroStateDirectiveLineInterval(directive, ownerPath, ownerBytes,
                                              directive.ownerIncludeId)
      .has_value();
}

bool RefoldHeaderIncludeEditPlanner::
    CollectMacroStatePreservationsFromIncludeSubtree(
        const RefoldModel::IncludeItem &root, StringRef replacement,
        SmallVectorImpl<HeaderPreservedGapPiece> &out) const {
  SmallVector<HeaderPreservedGapPiece, 4> pieces;
  for (const auto &directive : model_.GetMacroDirectives()) {
    if (directive.subkind != "#define" && directive.subkind != "#undef")
      continue;
    if (!IncludeOwnsDirective(directive, root))
      continue;

    if (macroStateProof_.ReplacementObservesMacroStateDirective(
            directive, replacement, /*unparseableObserves=*/true))
      return false;
    if (!RecordedMacroDirectiveMatchesOwnerFile(directive))
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
}

bool RefoldHeaderIncludeEditPlanner::
    TryBuildConsumedHeaderConditionalCoalescedPatch(
        const IncludeEdits &includeEdits, StringRef file, StringRef headerText,
        size_t idx, IncludePatch &coalesced, size_t &skipThrough) const {
  if (idx + 1 >= includeEdits.patches.size())
    return false;

  const IncludePatch &first = includeEdits.patches[idx];
  if (first.hasDirectHeaderByteRange)
    return false;
  if (first.aStart >= first.aEnd)
    return false;

  // Some LCS shapes preserve a token from inside a selected conditional arm
  // while adjacent hunks delete the rest of that arm. Merge the shortest
  // adjacent patch run that covers a complete selected header conditional group
  // so the normal source-envelope proof can consume the group as one piece.
  for (size_t endIdx = idx + 1; endIdx < includeEdits.patches.size();
       ++endIdx) {
    const IncludePatch &last = includeEdits.patches[endIdx];
    if (last.include != first.include)
      break;
    if (last.condArm.present != first.condArm.present)
      break;
    if (last.condArm.present && last.condArm.armId != first.condArm.armId)
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
      if (!HeaderConditionalGroupSelectedMaterialOverlaps(
              *includeEdits.include, file, group, materialBeginA, materialEndA))
        continue;
      overlapsConditionalGroup = true;
      if (HeaderConditionalGroupIsConsumedSourceEnvelope(
              *includeEdits.include, file, headerText, group, materialBeginA,
              materialEndA)) {
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
    coalesced.insertBytes =
        refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                      coalesced.bStart, coalesced.bEnd)
            .str();
    skipThrough = endIdx;
    return true;
  }

  return false;
}

bool RefoldHeaderIncludeEditPlanner::TryBuildMaterialChildStateCoalescedPatch(
    const IncludeEdits &includeEdits, StringRef file, size_t idx,
    IncludePatch &coalesced, size_t &skipThrough) const {
  if (idx + 1 >= includeEdits.patches.size())
    return false;

  const IncludePatch &first = includeEdits.patches[idx];
  const IncludePatch &second = includeEdits.patches[idx + 1];
  if (first.hasDirectHeaderByteRange || second.hasDirectHeaderByteRange)
    return false;
  if (first.include != second.include)
    return false;
  if (first.condArm.present != second.condArm.present)
    return false;
  if (first.condArm.present && first.condArm.armId != second.condArm.armId)
    return false;
  if (first.aStart >= first.aEnd || second.aStart >= second.aEnd)
    return false;
  if (second.aEnd <= first.aStart || second.bEnd < first.bStart)
    return false;

  for (const auto &child : model_.GetIncludes()) {
    if (!child.parent || *child.parent != includeEdits.include->id ||
        !paths_.PathsEqual(child.sitePath, file) || !child.cover.IsValid())
      continue;
    if (child.cover.end <= child.cover.begin)
      continue;

    if (!(first.aStart < child.cover.begin && child.cover.begin < first.aEnd &&
          first.aEnd <= second.aStart && second.aStart < child.cover.end &&
          child.cover.end < second.aEnd))
      continue;

    uint64_t prefix = 0;
    while (first.aStart + prefix < child.cover.begin &&
           first.bStart + prefix < second.bEnd &&
           tokenRangesHaveSameSpelling(
               aToks_, bToks_, first.aStart + prefix, first.aStart + prefix + 1,
               first.bStart + prefix, first.bStart + prefix + 1)) {
      ++prefix;
    }

    if (first.aStart + prefix >= child.cover.begin ||
        first.bStart + prefix > second.bEnd)
      continue;

    const uint64_t newAStart = first.aStart + prefix;
    const uint64_t newBStart = first.bStart + prefix;
    std::string newInsert =
        refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_, newBStart,
                                      second.bEnd)
            .str();

    SmallVector<HeaderPreservedGapPiece, 4> statePieces;
    if (!CollectMacroStatePreservationsFromIncludeSubtree(
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
}

bool RefoldHeaderIncludeEditPlanner::
    HeaderMacroInvocationIsConsumedSourceEnvelope(
        const RefoldModel::IncludeItem &currentInclude, StringRef file,
        StringRef headerText, const RefoldModel::MacroInvocation &macro,
        uint64_t materialBeginA, uint64_t materialEndA) const {
  if (!macro.invFile || macro.invFile->empty() ||
      !paths_.PathsEqual(*macro.invFile, file))
    return false;
  if (!macro.ownerIncludeId || *macro.ownerIncludeId != currentInclude.id)
    return false;
  if (!macro.invB || !macro.invE || *macro.invB >= *macro.invE ||
      *macro.invE > headerText.size())
    return false;
  if (macro.invText &&
      headerText.slice(*macro.invB, *macro.invE) != *macro.invText)
    return false;

  bool sawMaterializedSpan = false;
  auto checkSpan = [&](const RefoldModel::PPSpan &span) {
    if (!span.IsValid() || span.begin >= span.end)
      return true;
    sawMaterializedSpan = true;
    return PPSpanInsideHeaderMaterial(span, materialBeginA, materialEndA);
  };

  // A source-bearing macro callsite is consumable only when every PP range the
  // producer attributes to the invocation is covered by the replacement
  // material. This prevents deleting a callsite whose expansion still has
  // surviving output elsewhere in B.
  for (const auto &span : macro.spans)
    if (!checkSpan(span))
      return false;
  for (const auto &span : macro.bodySpans)
    if (!checkSpan(span))
      return false;
  for (const auto &span : macro.argSpans)
    if (!checkSpan(span))
      return false;
  for (const auto &span : macro.stringifySpans)
    if (!checkSpan(span))
      return false;
  for (const auto &span : macro.pasteSpans)
    if (!checkSpan(span))
      return false;

  return sawMaterializedSpan;
}

bool RefoldHeaderIncludeEditPlanner::HeaderMacroInvocationOverlapsMaterial(
    const RefoldModel::IncludeItem &currentInclude, StringRef file,
    const RefoldModel::MacroInvocation &macro, uint64_t materialBeginA,
    uint64_t materialEndA) const {
  if (!macro.invFile || macro.invFile->empty() ||
      !paths_.PathsEqual(*macro.invFile, file))
    return false;
  if (!macro.ownerIncludeId || *macro.ownerIncludeId != currentInclude.id)
    return false;
  if (!macro.cover.IsValid() || macro.cover.begin >= macro.cover.end)
    return false;
  return materialBeginA < macro.cover.end && macro.cover.begin < materialEndA;
}

bool RefoldHeaderIncludeEditPlanner::HeaderLineDirectiveStartsAtPrefix(
    StringRef headerText, uint64_t pos) {
  if (pos >= headerText.size())
    return true;
  if (stringutils::isBOL(headerText, static_cast<size_t>(pos)))
    return true;

  size_t lineStart = static_cast<size_t>(pos);
  while (lineStart > 0 && headerText[lineStart - 1] != '\n')
    --lineStart;
  return stringutils::isIndentOnly(headerText, lineStart,
                                   static_cast<size_t>(pos));
}

bool RefoldHeaderIncludeEditPlanner::
    CanStartHeaderLineDirectiveWithOptionalLeadingNewline(StringRef headerText,
                                                          uint64_t pos) {
  if (HeaderLineDirectiveStartsAtPrefix(headerText, pos))
    return true;
  return pos == 0 || headerText[pos - 1] != '\\';
}

bool RefoldHeaderIncludeEditPlanner::
    ReplacementHeaderSuffixBoundaryAllowsDirectiveLine(
        StringRef headerText, StringRef replacement, uint64_t sourceEnd) const {
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

  // Whitespace on either side is already a lexical separator. Otherwise, fall
  // back to token-boundary checking so widening the edit cannot glue two
  // neighboring tokens into a different token spelling.
  if (replacement.empty() || stringutils::isWs(replacement.back()))
    return true;
  if (sourceEnd >= headerText.size() ||
      stringutils::isWs(headerText[sourceEnd]))
    return true;

  std::optional<RefoldLexBoundaryToken> leftTok =
      refoldLastLexToken(replacement, lexLang_);
  std::optional<RefoldLexBoundaryToken> rightTok =
      refoldFirstLexToken(headerText.drop_front(sourceEnd), lexLang_);
  if (!leftTok || !rightTok)
    return true;

  return !refoldNeedsLexicalSeparator(*leftTok, *rightTok, lexLang_);
}

bool RefoldHeaderIncludeEditPlanner::HeaderSourceEnvelopePieceKindPrecedes(
    const HeaderSourceEnvelopePiece &lhs,
    const HeaderSourceEnvelopePiece &rhs) {
  return lhs.kind < rhs.kind;
}

bool RefoldHeaderIncludeEditPlanner::HeaderSourceEnvelopePieceContains(
    const HeaderSourceEnvelopePiece &outer,
    const HeaderSourceEnvelopePiece &piece) {
  if (outer.kind == "child-include" && piece.kind == "token") {
    // Tokens produced by a child include are owned by the child site. Once the
    // complete child include is a source-envelope piece, any mapped child token
    // inside that site is not a second parent piece.
    return true;
  }
  if (outer.kind == "macro-invocation" && piece.kind == "token") {
    // Macro-expanded tokens often map to the macro callsite spelling. Once the
    // complete source-bearing invocation is proved consumed, those mapped token
    // intervals are internal evidence, not separate parent-header pieces.
    return true;
  }
  if (outer.kind == "conditional-group" &&
      (piece.kind == "token" || piece.kind == "macro-invocation" ||
       piece.kind == "child-include")) {
    // A consumed conditional group owns the selected-arm source inside its
    // directive wrapper. Nested token/macro/include pieces inside the group are
    // covered by the group-level source-envelope proof.
    return true;
  }
  return false;
}

bool RefoldHeaderIncludeEditPlanner::HeaderPreservedGapPieceKindPrecedes(
    const HeaderPreservedGapPiece &lhs, const HeaderPreservedGapPiece &rhs) {
  return static_cast<unsigned>(lhs.kind) < static_cast<unsigned>(rhs.kind);
}

bool RefoldHeaderIncludeEditPlanner::HeaderPreservedGapPieceContains(
    const HeaderPreservedGapPiece &outer,
    const HeaderPreservedGapPiece &piece) {
  if (outer.kind == HeaderPreservedGapPiece::Kind::ZeroTokenMacroInvocation &&
      piece.kind == HeaderPreservedGapPiece::Kind::ZeroTokenMacroInvocation) {
    // Nested zero-token macro invocations are already part of the outer
    // invocation's source-neutral proof. Preserve only the outer callsite text
    // so the refolded header does not duplicate the same gap structure.
    return true;
  }

  if (outer.kind == HeaderPreservedGapPiece::Kind::ZeroTokenConditionalGroup) {
    // A preserved complete conditional group owns every artifact inside its
    // source island after the shared conditional-island proof has discharged
    // those artifacts.
    return true;
  }

  return false;
}

bool RefoldHeaderIncludeEditPlanner::HeaderGapRangeIsTrivia(
    StringRef headerText, uint64_t begin, uint64_t end) {
  return isWsOrCompleteCommentTrivia(headerText.slice(begin, end));
}

bool RefoldHeaderIncludeEditPlanner::ProveHeaderSourceEnvelopeGap(
    const HeaderSourceEnvelopePlanningState &state,
    const SourceEnvelopeInterval &sourceEnvelope, uint64_t gapBegin,
    uint64_t gapEnd,
    SmallVectorImpl<HeaderPreservedGapPiece> &preservedPieces) const {
  auto pathIdentityPathsEqual = [this](StringRef lhs, StringRef rhs) {
    return paths_.PathsEqual(lhs, rhs);
  };

  SourceLineDirectiveLogicalLineRewriter headerSourceLineDirectiveLineRewriter =
      [&](StringRef logicalLine, ArrayRef<uint64_t> sourceOffsets,
          SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver)
      -> std::optional<SourceLineDirectiveLogicalLineRewrite> {
    return rewriteSourceLineDirectiveLogicalLineMacros(
        model_, state.file, logicalLine, sourceOffsets, pathIdentityPathsEqual,
        lexLang_, state.include.id, std::move(builtinMacroResolver));
  };

  if (std::optional<SourceLineDirectiveGapResume> lineResume =
          computeSourceLineDirectiveGapResume(
              state.headerText, gapBegin, gapEnd, sourceEnvelope.end,
              state.file, headerSourceLineDirectiveLineRewriter, nullptr,
              model_.GetSourcePath(),
              !sourceSuffixMayObservePresumedFileSpelling(
                  model_, state.file, sourceEnvelope.end,
                  pathIdentityPathsEqual, state.headerText))) {
    // Header full-envelope widening uses the same owner-piece gap proof as
    // TU/include closure. A source-spelled line-control gap is not disposable
    // trivia, but it is preservable by carrying its net line state forward to
    // the untouched header suffix.
    state.headerSourceLineDirectiveResume = std::move(lineResume);
    return true;
  }

  SmallVector<HeaderPreservedGapPiece, 4> gapPieces;
  for (const auto &directive : model_.GetMacroDirectives()) {
    std::optional<MacroStateDirectiveLineInterval> directivePiece =
        macroStateProof_.RecoverMacroStateDirectiveLineInterval(
            directive, state.file, state.headerText,
            std::optional<uint64_t>(state.include.id));
    if (!directivePiece || directivePiece->begin < gapBegin ||
        gapEnd < directivePiece->end)
      continue;
    if (macroStateProof_.ReplacementObservesMacroStateDirective(
            *directivePiece->directive, state.materialInsertBytes,
            /*unparseableObserves=*/true))
      return false;

    HeaderPreservedGapPiece piece;
    piece.kind = HeaderPreservedGapPiece::Kind::MacroStateDirective;
    piece.directive = directivePiece->directive;
    piece.begin = directivePiece->begin;
    piece.end = directivePiece->end;
    piece.id = directivePiece->directive ? directivePiece->directive->id : 0;
    gapPieces.push_back(std::move(piece));
  }

  for (const auto &group : model_.GetConds()) {
    if (!HeaderConditionalGroupIsPreservableGap(
            state.headerSourceNeutrality, state.include, state.file,
            state.headerText, group, gapBegin, gapEnd))
      continue;
    HeaderPreservedGapPiece piece;
    piece.kind = HeaderPreservedGapPiece::Kind::ZeroTokenConditionalGroup;
    piece.begin = group.groupB;
    piece.end = group.groupE;
    piece.id = group.id;
    gapPieces.push_back(std::move(piece));
  }

  // Pragma/state proof for materialized header owners. A balanced diagnostic
  // pragma island is a source-state atom: the island may be preserved across a
  // header source gap only when push/pop depth returns to zero and every byte
  // crossed by the island is trivia.
  SmallVector<BalancedDiagnosticPragmaStateIsland, 4> pragmaIslands;
  collectBalancedDiagnosticPragmaStateIslands(
      model_, state.headerText, gapBegin, gapEnd,
      [&](const RefoldModel::PragmaDirective &pragma) {
        return paths_.PathsEqual(pragma.sitePath, state.file);
      },
      pragmaIslands, lexLang_);
  for (const BalancedDiagnosticPragmaStateIsland &island : pragmaIslands) {
    HeaderPreservedGapPiece piece;
    piece.kind = HeaderPreservedGapPiece::Kind::BalancedPragmaStateIsland;
    piece.begin = island.begin;
    piece.end = island.end;
    piece.id = island.id;
    gapPieces.push_back(std::move(piece));
  }

  for (const auto &macro : model_.GetMacroInvocations()) {
    if (!HeaderMacroInvocationIsPreservableGap(
            state.headerSourceNeutrality, state.include, state.file,
            state.headerText, macro, gapBegin, gapEnd))
      continue;
    HeaderPreservedGapPiece piece;
    piece.kind = HeaderPreservedGapPiece::Kind::ZeroTokenMacroInvocation;
    piece.begin = *macro.invB;
    piece.end = *macro.invE;
    piece.id = macro.id;
    gapPieces.push_back(std::move(piece));
  }

  for (const auto &child : model_.GetIncludes()) {
    if (!HeaderZeroTokenChildIncludeIsPreservableGap(
            state.include, state.file, state.headerText, child, gapBegin,
            gapEnd, state.materialInsertBytes))
      continue;
    HeaderPreservedGapPiece piece;
    piece.kind = HeaderPreservedGapPiece::Kind::ZeroTokenChildInclude;
    piece.begin = child.siteB;
    piece.end = child.siteE;
    piece.id = child.id;
    gapPieces.push_back(std::move(piece));
  }

  if (!proveSourceEnvelopeGap(
          gapPieces, gapBegin, gapEnd, HeaderPreservedGapPieceKindPrecedes,
          HeaderPreservedGapPieceContains,
          [&](uint64_t begin, uint64_t end) {
            return HeaderGapRangeIsTrivia(state.headerText, begin, end);
          },
          [](const HeaderPreservedGapPiece &) {}))
    return false;

  preservedPieces.append(gapPieces.begin(), gapPieces.end());
  return true;
}

bool RefoldHeaderIncludeEditPlanner::TryApplyDeleteReplaceSourceEnvelope(
    const HeaderSourceEnvelopePlanningState &state) const {
  // A single PP hunk can legitimately cross neighboring header-owned source
  // pieces when the bytes between the material tokens are complete zero-token
  // preprocessor structure. The ordinary mapped-token range is intentionally
  // local, but leaving the rest of the hunk in the header can preserve stale
  // source (`int`, a comma, or an include directive). Build a full source
  // envelope from header tokens and complete source pieces, then prove every
  // inter-piece gap before overriding the local range.
  const bool declClippedNonEmptyPatch =
      state.decl &&
      (state.materialAStart < state.ppLo || state.ppHi < state.materialAEnd);
  SmallVector<HeaderSourceEnvelopePiece, 8> hunkSourcePieces;
  bool hasCompleteChildIncludePiece = false;
  bool hasPartialChildIncludeOverlap = false;
  const uint64_t fullLo = std::max(state.materialAStart, state.coverBegin);
  const uint64_t fullHi = std::min(state.materialAEnd, state.coverEnd);

  const auto &tokmapByPP = model_.GetTokmapByPP();
  for (uint64_t pp = fullLo; pp < fullHi; ++pp) {
    auto it = tokmapByPP.find(pp);
    if (it == tokmapByPP.end() ||
        !paths_.PathsEqual(it->second.file, state.file))
      continue;

    // Require exact token byte bounds here: widening across header source
    // pieces needs precise endpoints on both sides of each gap.
    std::optional<uint64_t> tokBegin =
        sourceMapper_.ByteStartForPPInFile(state.file, pp);
    std::optional<uint64_t> tokEnd =
        sourceMapper_.ByteEndForPPInFile(state.file, pp);
    if (!tokBegin || !tokEnd || *tokBegin >= *tokEnd) {
      hunkSourcePieces.clear();
      break;
    }
    hunkSourcePieces.push_back({*tokBegin, *tokEnd, pp, pp + 1, pp, "token"});
  }

  for (const auto &child : model_.GetIncludes()) {
    if (!child.parent || *child.parent != state.include.id ||
        !paths_.PathsEqual(child.sitePath, state.file) ||
        !child.cover.IsValid())
      continue;
    if (child.cover.end <= fullLo || fullHi <= child.cover.begin)
      continue;

    if (fullLo <= child.cover.begin && child.cover.end <= fullHi &&
        child.siteB < child.siteE && child.siteE <= state.headerText.size()) {
      hasCompleteChildIncludePiece = true;
      hunkSourcePieces.push_back({child.siteB, child.siteE, child.cover.begin,
                                  child.cover.end, child.id, "child-include"});
      continue;
    }

    // A partial child-include overlap would require editing inside the child
    // include and its parent directive at the same time. Leave that to include
    // realization rather than inventing a parent-local range.
    hasPartialChildIncludeOverlap = true;
  }

  bool hasCompleteMacroInvocationPiece = false;
  bool hasCompleteConditionalGroupPiece = false;
  bool hasPartialMacroInvocationOverlap = false;
  bool hasPartialConditionalGroupOverlap = false;

  for (const auto &macro : model_.GetMacroInvocations()) {
    if (!HeaderMacroInvocationOverlapsMaterial(state.include, state.file, macro,
                                               fullLo, fullHi))
      continue;
    if (HeaderMacroInvocationIsConsumedSourceEnvelope(state.include, state.file,
                                                      state.headerText, macro,
                                                      fullLo, fullHi)) {
      hasCompleteMacroInvocationPiece = true;
      hunkSourcePieces.push_back({*macro.invB, *macro.invE, macro.cover.begin,
                                  macro.cover.end, macro.id,
                                  "macro-invocation"});
      continue;
    }

    // A partial source-bearing macro overlap means the parent header edit would
    // delete only part of the callsite's produced material. That is not a
    // source-envelope proof; require a stronger/coalesced candidate.
    hasPartialMacroInvocationOverlap = true;
  }

  for (const auto &group : model_.GetConds()) {
    if (!HeaderConditionalGroupSelectedMaterialOverlaps(
            state.include, state.file, group, fullLo, fullHi))
      continue;
    if (HeaderConditionalGroupIsConsumedSourceEnvelope(
            state.include, state.file, state.headerText, group, fullLo,
            fullHi)) {
      hasCompleteConditionalGroupPiece = true;
      hunkSourcePieces.push_back({group.groupB, group.groupE, fullLo, fullHi,
                                  group.id, "conditional-group"});
      continue;
    }

    // The hunk touches selected conditional output but does not consume the
    // selected arm. Do not widen across the directive wrapper unless a
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
    std::optional<SourceEnvelopeInterval> sourceEnvelope;
    if (hunkSourcePieces.size() >= 2 &&
        normalizeSourceEnvelopePieces(hunkSourcePieces,
                                      HeaderSourceEnvelopePieceKindPrecedes,
                                      HeaderSourceEnvelopePieceContains))
      sourceEnvelope = {sourceEnvelopePieceBegin(hunkSourcePieces.front()),
                        sourceEnvelopePieceEnd(hunkSourcePieces.back())};

    SmallVector<HeaderPreservedGapPiece, 4> preservedPieces;
    if (sourceEnvelope) {
      for (const HeaderSourceEnvelopePiece &piece : hunkSourcePieces) {
        if (piece.kind != "child-include")
          continue;
        const RefoldModel::IncludeItem *child = model_.GetIncludeById(piece.id);
        if (!child)
          continue;
        if (!CollectMacroStatePreservationsFromIncludeSubtree(
                *child, state.materialInsertBytes, preservedPieces)) {
          sourceEnvelope.reset();
          break;
        }
      }
    }

    if (sourceEnvelope &&
        !proveSourceEnvelopeGaps(hunkSourcePieces, [&](uint64_t gapBegin,
                                                       uint64_t gapEnd) {
          return ProveHeaderSourceEnvelopeGap(state, *sourceEnvelope, gapBegin,
                                              gapEnd, preservedPieces);
        }))
      sourceEnvelope.reset();

    if (sourceEnvelope) {
      const bool hasHeaderSuffixAfterSourceLineDirective =
          state.headerSourceLineDirectiveResume &&
          sourceEnvelope->end < state.headerText.size();
      const bool canRestoreSourceLineState =
          !state.headerSourceLineDirectiveResume ||
          (!hasHeaderSuffixAfterSourceLineDirective ||
           (lineDirs_.Enabled() &&
            HeaderLineDirectiveStartsAtPrefix(state.headerText,
                                              sourceEnvelope->end) &&
            (!state.isDelete ||
             CanStartHeaderLineDirectiveWithOptionalLeadingNewline(
                 state.headerText, sourceEnvelope->begin))));

      if (sourceEnvelope->begin <= *state.startByte &&
          *state.endByte <= sourceEnvelope->end && canRestoreSourceLineState &&
          ReplacementHeaderSuffixBoundaryAllowsDirectiveLine(
              state.headerText, state.materialInsertBytes,
              sourceEnvelope->end)) {
        state.startByte = sourceEnvelope->begin;
        state.endByte = sourceEnvelope->end;
        state.headerGapPreservations.clear();
        state.headerGapPreservations.append(preservedPieces.begin(),
                                            preservedPieces.end());
        state.usedFullHeaderEnvelope = true;
        state.mappedHeaderWitness.startByte = sourceEnvelope->begin;
        state.mappedHeaderWitness.endByte = sourceEnvelope->end;
        state.mappedHeaderWitness.firstPP = hunkSourcePieces.front().ppBegin;
        state.mappedHeaderWitness.lastPP = hunkSourcePieces.back().ppEnd - 1;
      }
    }
  }

  // If the replacement material intersects complete child source structure
  // owned by this header, a plain mapped-token edit is not a valid proof: it
  // can replace only the directly mapped parent tokens and leave stale
  // neighboring macro/include syntax behind. Either the edit widened to the
  // proven source envelope above, or this include must be realized from B.
  if (fullHeaderEnvelopeBlockedByPartialOverlap) {
    state.plan.requiresIncludeRealization = true;
    state.plan.realizationReason =
        llvm::formatv("DELETE/REPLACE: patch[{0}] in header file {1} "
                      "partially overlaps source-envelope structure",
                      state.patchIndex, state.file)
            .str();
    return false;
  }
  if (mustUseFullHeaderEnvelope && !state.usedFullHeaderEnvelope) {
    state.plan.requiresIncludeRealization = true;
    state.plan.realizationReason =
        llvm::formatv("DELETE/REPLACE: patch[{0}] in header file {1} "
                      "requires a full source-envelope proof",
                      state.patchIndex, state.file)
            .str();
    return false;
  }

  return true;
}

bool RefoldHeaderIncludeEditPlanner::ActiveHeaderDefinitionAtByte(
    const RefoldModel::MacroDirective &definition, StringRef macroName,
    const RefoldModel::IncludeItem &include, StringRef file,
    StringRef headerText, uint64_t offset) const {
  // The carry proof applies only to the definition that is active at the
  // replacement start. Re-emitting a shadowed definition after the B payload
  // would synthesize a macro state that never existed at this point.
  const RefoldModel::MacroDirective *active = nullptr;
  uint64_t activeEnd = 0;
  for (const auto &candidate : model_.GetMacroDirectives()) {
    std::optional<MacroStateDirectiveLineInterval> piece =
        macroStateProof_.RecoverMacroStateDirectiveLineInterval(
            candidate, file, headerText, std::optional<uint64_t>(include.id));
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
}

bool RefoldHeaderIncludeEditPlanner::HeaderRangeOverlapsStagedEdit(
    ArrayRef<TextEdit> edits, uint64_t begin, uint64_t end) {
  for (const TextEdit &edit : edits)
    if (begin < edit.end && end > edit.start)
      return true;
  return false;
}

bool RefoldHeaderIncludeEditPlanner::HeaderMacroStateCarryCandidatePrecedes(
    const HeaderMacroStateCarryCandidate &lhs,
    const HeaderMacroStateCarryCandidate &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  return lhs.directive->id < rhs.directive->id;
}

void RefoldHeaderIncludeEditPlanner::CollectHeaderMacroStateCarryCandidates(
    const HeaderMacroStateCarryState &state,
    SmallVectorImpl<HeaderMacroStateCarryCandidate> &out) const {
  out.clear();
  if (!state.startByte || !state.endByte)
    return;

  // Collect active header-owned #define directives that currently precede this
  // header-local replacement and that the B-derived replacement would observe
  // if they stayed there. Those are exactly the definitions whose source-order
  // position must be reconsidered to make the replacement see B's macro state.
  for (const auto &directive : model_.GetMacroDirectives()) {
    if (directive.subkind != "#define")
      continue;

    std::optional<MacroStateDirectiveLineInterval> piece =
        macroStateProof_.RecoverMacroStateDirectiveLineInterval(
            directive, state.file, state.headerText,
            std::optional<uint64_t>(state.include.id));
    if (!piece || piece->end > *state.startByte)
      continue;
    if (HeaderRangeOverlapsStagedEdit(state.plan.edits, piece->begin,
                                      piece->end))
      continue;
    if (!ActiveHeaderDefinitionAtByte(directive, piece->name, state.include,
                                      state.file, state.headerText,
                                      *state.startByte))
      continue;
    if (!macroStateProof_.ReplacementObservesMacroStateDirective(
            directive, StringRef(state.replacement),
            /*unparseableObserves=*/false))
      continue;

    // Header materialization has the same placement-sensitive macro-state
    // invariant as TU gap carry. We may carry a definition after the
    // replacement only when the crossed original bytes do not themselves
    // observe it.
    if (macroStateProof_.SourceChunkObservesMacroStateDirectiveWhenCrossed(
            directive, piece->name,
            state.headerText.slice(piece->end, *state.startByte),
            state.headerText.slice(*state.startByte, *state.endByte)))
      continue;

    out.push_back(HeaderMacroStateCarryCandidate{&directive, piece->begin,
                                                 piece->end, piece->name});
  }
}

bool RefoldHeaderIncludeEditPlanner::HeaderMacroStateCarryCandidatesCanCross(
    const HeaderMacroStateCarryState &state,
    ArrayRef<HeaderMacroStateCarryCandidate> candidates) const {
  if (candidates.empty() || !state.startByte)
    return false;

  uint64_t cursor = candidates.front().begin;
  for (const HeaderMacroStateCarryCandidate &candidate : candidates) {
    if (cursor > candidate.begin)
      return false;

    StringRef crossed = state.headerText.slice(cursor, candidate.begin);
    StringRef following =
        state.headerText.slice(candidate.begin, *state.startByte);
    // Once an earlier definition is carried, every byte crossed before the next
    // carried directive must remain non-observing for that prior definition.
    for (const HeaderMacroStateCarryCandidate &prior : candidates) {
      if (prior.end <= cursor)
        continue;
      if (prior.begin >= candidate.begin)
        break;
      if (macroStateProof_.SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *prior.directive, prior.name, crossed, following))
        return false;
    }
    cursor = candidate.end;
  }
  return true;
}

uint64_t
RefoldHeaderIncludeEditPlanner::HeaderMacroStateCarryReplacementTailEnd(
    StringRef headerText, uint64_t endByte) {
  uint64_t replacementTailEnd = endByte;
  if (replacementTailEnd < headerText.size() &&
      !stringutils::isBOL(headerText,
                          static_cast<size_t>(replacementTailEnd))) {
    size_t nl = headerText.find('\n', replacementTailEnd);
    replacementTailEnd = nl == StringRef::npos
                             ? static_cast<uint64_t>(headerText.size())
                             : static_cast<uint64_t>(nl + 1);
  }
  return replacementTailEnd;
}

bool RefoldHeaderIncludeEditPlanner::HeaderMacroStateCarryTailIsNonObserving(
    const HeaderMacroStateCarryState &state,
    ArrayRef<HeaderMacroStateCarryCandidate> candidates,
    uint64_t replacementTailEnd) const {
  if (!state.endByte || replacementTailEnd <= *state.endByte)
    return true;

  StringRef tail = state.headerText.slice(*state.endByte, replacementTailEnd);
  StringRef following = state.headerText.drop_front(replacementTailEnd);
  for (const HeaderMacroStateCarryCandidate &candidate : candidates) {
    if (macroStateProof_.SourceChunkObservesMacroStateDirectiveWhenCrossed(
            *candidate.directive, candidate.name, tail, following))
      return false;
  }
  return true;
}

std::optional<RefoldHeaderIncludeEditPlanner::HeaderMacroStateCarryRewrite>
RefoldHeaderIncludeEditPlanner::BuildCarriedHeaderMacroStateRewrite(
    const HeaderMacroStateCarryState &state,
    ArrayRef<HeaderMacroStateCarryCandidate> candidates,
    uint64_t replacementTailEnd) const {
  if (candidates.empty() || !state.startByte || !state.endByte)
    return std::nullopt;

  const uint64_t newStart = candidates.front().begin;
  std::string carriedReplacement;
  carriedReplacement.reserve((*state.startByte - newStart) +
                             state.replacement.size() +
                             (replacementTailEnd - *state.endByte) + 64);

  // Rebuild the widened header edit as preserved bytes before carried
  // directives, B-derived replacement payload, optional non-observing physical
  // line tail, then the carried #define directive lines.
  uint64_t cursor = newStart;
  for (const HeaderMacroStateCarryCandidate &candidate : candidates) {
    if (cursor > candidate.begin)
      return std::nullopt;
    carriedReplacement.append(state.headerText.begin() + cursor,
                              state.headerText.begin() + candidate.begin);
    cursor = candidate.end;
  }

  carriedReplacement.append(state.headerText.begin() + cursor,
                            state.headerText.begin() + *state.startByte);
  carriedReplacement += state.replacement;
  if (replacementTailEnd > *state.endByte) {
    carriedReplacement.append(state.headerText.begin() + *state.endByte,
                              state.headerText.begin() + replacementTailEnd);
  } else if (!carriedReplacement.empty() && carriedReplacement.back() != '\n') {
    carriedReplacement.push_back('\n');
  }
  for (const HeaderMacroStateCarryCandidate &candidate : candidates) {
    carriedReplacement.append(candidate.directive->text.begin(),
                              candidate.directive->text.end());
    if (carriedReplacement.empty() || carriedReplacement.back() != '\n')
      carriedReplacement.push_back('\n');
  }

  return HeaderMacroStateCarryRewrite{newStart, replacementTailEnd,
                                      std::move(carriedReplacement)};
}

void RefoldHeaderIncludeEditPlanner::TryCarryHeaderMacroStateAfterReplacement(
    const HeaderMacroStateCarryState &state) const {
  if (!state.startByte || !state.endByte || *state.startByte > *state.endByte ||
      state.replacement.empty())
    return;
  if (!ReplacementHeaderSuffixBoundaryAllowsDirectiveLine(
          state.headerText, StringRef(state.replacement), *state.endByte))
    return;

  SmallVector<HeaderMacroStateCarryCandidate, 4> carryCandidates;
  CollectHeaderMacroStateCarryCandidates(state, carryCandidates);
  if (carryCandidates.empty())
    return;

  llvm::sort(carryCandidates, HeaderMacroStateCarryCandidatePrecedes);
  if (!HeaderMacroStateCarryCandidatesCanCross(state, carryCandidates))
    return;

  const uint64_t replacementTailEnd =
      HeaderMacroStateCarryReplacementTailEnd(state.headerText, *state.endByte);
  if (!HeaderMacroStateCarryTailIsNonObserving(state, carryCandidates,
                                               replacementTailEnd))
    return;

  std::optional<HeaderMacroStateCarryRewrite> rewrite =
      BuildCarriedHeaderMacroStateRewrite(state, carryCandidates,
                                          replacementTailEnd);
  if (!rewrite)
    return;

  *state.startByte = rewrite->startByte;
  *state.endByte = rewrite->endByte;
  state.replacement = std::move(rewrite->replacement);
  state.mappedHeaderWitness.hasByteRange = true;
  state.mappedHeaderWitness.startByte = *state.startByte;
  state.mappedHeaderWitness.endByte = *state.endByte;
}

void RefoldHeaderIncludeEditPlanner::
    CheckHeaderSourceLineResumeStateTransitions(
        const IncludeEdits &includeEdits, size_t patchIndex,
        uint64_t sourceEnd) const {
  const std::string file =
      refoldIncludeEnteredFileSpelling(*includeEdits.include);
  const OwnerStateBoundary sourceLineResumeBoundary =
      OwnerStateBoundary::FromSource(OwnerSourceRange::From(
          file, sourceEnd, sourceEnd,
          std::optional<uint64_t>(includeEdits.include->id)));

  const OwnerStateComponent components[] = {OwnerStateComponent::LineNumber,
                                            OwnerStateComponent::FileState,
                                            OwnerStateComponent::FileName};
  for (OwnerStateComponent component : components) {
    const std::string detail =
        llvm::formatv("synthetic header source #line resume for include "
                      "#{0} patch[{1}] component={2} sourceEnd={3}",
                      includeEdits.include->id, patchIndex, component,
                      sourceEnd)
            .str();
    (void)ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
        sourceLineResumeBoundary, component, StateMutationKind::Replayed,
        ownerStateProof_.BuildStateTransitionWitness(
            SuffixStabilityWitnessKind::StateRepair, component,
            sourceLineResumeBoundary, detail),
        "include/source-line-resume", detail,
        /*requireKnownObserver=*/false);
  }
}

void RefoldHeaderIncludeEditPlanner::
    NormalizeDuplicatedBoundaryTokenAcrossLineControlGap(
        StringRef file, StringRef headerText, uint64_t ppHi,
        uint64_t &materialAStart, uint64_t &materialBStart,
        uint64_t &materialBEnd, uint64_t &materialInsertPos,
        std::string &materialInsertBytes) const {
  if (materialAStart >= static_cast<uint64_t>(aToks_.size()))
    return;
  if (materialBStart >= materialBEnd ||
      materialBEnd >= static_cast<uint64_t>(bToks_.size()))
    return;
  if (abTokMapA2B_.empty() ||
      materialAStart >= static_cast<uint64_t>(abTokMapA2B_.size()))
    return;

  // The original A boundary token is currently matched to the B token
  // immediately after the insertion island. Without this witness, there is no
  // duplicated-boundary-token tie to normalize.
  if (abTokMapA2B_[static_cast<size_t>(materialAStart)] !=
      static_cast<int64_t>(materialBEnd))
    return;
  if (aToks_[static_cast<size_t>(materialAStart)].spelling !=
          bToks_[static_cast<size_t>(materialBStart)].spelling ||
      aToks_[static_cast<size_t>(materialAStart)].spelling !=
          bToks_[static_cast<size_t>(materialBEnd)].spelling)
    return;

  const auto &tokmapByPP = model_.GetTokmapByPP();
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

  // Examine owner-local source bytes between the retained source boundary token
  // and the next copied owner token. Only a source line-control directive makes
  // this token tie semantically observable for placement.
  std::optional<uint64_t> leftEnd =
      sourceMapper_.ByteEndForPPInFile(file, materialAStart);
  std::optional<uint64_t> rightBegin =
      sourceMapper_.ByteStartForPPInFile(file, *nextOwnerPP);
  if (!leftEnd || !rightBegin || *rightBegin < *leftEnd)
    return;
  if (!sourceIntervalContainsLineControlDirective(headerText, *leftEnd,
                                                  *rightBegin))
    return;

  const uint64_t normalizedBStart = materialBStart + 1;
  const uint64_t normalizedBEnd = materialBEnd + 1;
  if (normalizedBStart >= normalizedBEnd ||
      normalizedBEnd > static_cast<uint64_t>(bTokOff_.size()))
    return;

  // Drop the duplicated leading B token from the island and include the later B
  // token that closed the island. The B token envelope preserves separator
  // bytes between the inserted payload and the copied suffix.
  materialBStart = normalizedBStart;
  materialBEnd = normalizedBEnd;
  materialInsertPos = *nextOwnerPP;
  materialInsertBytes =
      refoldSliceTokenEnvelope(bTokOff_, bSource_, materialBStart, materialBEnd)
          .str();
}

bool RefoldHeaderIncludeEditPlanner::AnchorMatchesCondArmCert(
    const HeaderInsertionPlanningState &state, uint64_t anchorByte) const {
  const IncludePatch &patch = state.patch;
  if (!patch.condArm.present)
    return true;

  auto armRef =
      model_.FindArmRefForByte(state.file, state.include.id, anchorByte);
  return armRef && armRef->arm && armRef->arm->id == patch.condArm.armId;
}

std::optional<RefoldHeaderIncludeEditPlanner::SelectedInsertAnchorCandidate>
RefoldHeaderIncludeEditPlanner::SelectBestInsertCandidate(
    ArrayRef<InsertAnchorCandidate> candidates,
    const IncludePatch &patch) const {
  if (candidates.empty())
    return std::nullopt;

  SmallVector<AcceptedResultCandidate, 4> acceptedCandidates;
  acceptedCandidates.reserve(candidates.size());
  for (const InsertAnchorCandidate &candidate : candidates) {
    acceptedCandidates.push_back(
        proofLattice_.AcceptedCandidateBuilder().BuildAcceptedIncludeCandidate(
            candidate.path, patch, &candidate.witness));
  }

  const std::optional<SelectedAcceptedResultCandidate> selected =
      proofLattice_.AcceptedResultRanker()
          .SelectPreferredAcceptedResultCandidate(acceptedCandidates);
  if (!selected)
    return std::nullopt;

  SelectedInsertAnchorCandidate result;
  result.anchor = candidates[selected->index];
  result.accepted = selected->candidate;
  return result;
}

void RefoldHeaderIncludeEditPlanner::CommitInsertCandidate(
    const HeaderInsertionPlanningState &state,
    const SelectedInsertAnchorCandidate &selected) const {
  const InsertAnchorCandidate &candidate = selected.anchor;
  std::string text = refoldPadAtBoundaries(
      state.headerText, static_cast<size_t>(candidate.anchorByte),
      static_cast<size_t>(candidate.anchorByte),
      state.materialInsertBytes.str(), /*allowLeft=*/true, /*allowRight=*/true,
      lexLang_);

  const bool needsLocalResync =
      InsertionNeedsLocalResync(state, candidate, text);
  TextEdit edit = needsLocalResync ? MakeTextEditWithResyncOrPending(
                                         state.headerText, candidate.anchorByte,
                                         candidate.anchorByte, text, state.file,
                                         state.include.id)
                                   : TextEdit{candidate.anchorByte,
                                              candidate.anchorByte,
                                              std::move(text),
                                              std::nullopt,
                                              std::nullopt,
                                              {},
                                              {},
                                              {}};
  textEditAssembler_.AttachAcceptedResultCarrier(edit, selected.accepted);
  state.plan.edits.push_back(std::move(edit));
}

bool RefoldHeaderIncludeEditPlanner::HasCopiedHeaderSuffix(
    StringRef headerText, uint64_t fileLen, uint64_t anchorByte) {
  for (uint64_t byte = anchorByte; byte < fileLen; ++byte)
    if (!stringutils::isWs(headerText[static_cast<size_t>(byte)]))
      return true;
  return false;
}

const RefoldModel::IncludeItem *
RefoldHeaderIncludeEditPlanner::ChildIncludeAtBoundaryAnchor(
    const RefoldModel::IncludeItem &currentInclude, StringRef file,
    uint64_t anchorByte) const {
  for (const auto &child : model_.GetIncludes()) {
    if (!child.parent || *child.parent != currentInclude.id ||
        !paths_.PathsEqual(child.sitePath, file))
      continue;
    if (child.siteB == anchorByte || child.siteE == anchorByte)
      return &child;
  }
  return nullptr;
}

bool RefoldHeaderIncludeEditPlanner::IncludeHasVisibleSidebandWork(
    uint64_t includeId) const {
  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits_)
    if (sideband.TargetsInclude(includeId) && sideband.EmitsVisibleReplayText())
      return true;
  return false;
}

bool RefoldHeaderIncludeEditPlanner::InsertionNeedsLocalResync(
    const HeaderInsertionPlanningState &state,
    const InsertAnchorCandidate &candidate, StringRef replacement) const {
  const bool copiedHeaderSuffix = HasCopiedHeaderSuffix(
      state.headerText, state.fileLen, candidate.anchorByte);
  const RefoldModel::IncludeItem *boundaryChild = ChildIncludeAtBoundaryAnchor(
      state.include, state.file, candidate.anchorByte);
  const bool boundaryChildWillBeMaterialized =
      boundaryChild && IncludeHasVisibleSidebandWork(boundaryChild->id);
  const bool suffixObservesLineState =
      lineControlProof_.OwnerSuffixHasLineStateSensitiveBuiltin(
          state.include.id, state.file, candidate.anchorByte);
  const bool suffixStartsDirective = suffixBeginsWithPreprocessorDirective(
      state.headerText, candidate.anchorByte);
  const bool replayCarriesDirectiveLine =
      replayContainsPreprocessorDirectiveLine(replacement);

  // A pure insertion at physical header EOF has no copied header suffix whose
  // logical location must be restored inside this owner.  For a child-include
  // boundary, suppress the local parent resync only when the child is itself
  // being materialized by a proved sideband replay; the child wrapper then
  // performs the transition immediately.
  return copiedHeaderSuffix &&
         (suffixObservesLineState || suffixStartsDirective ||
          replayCarriesDirectiveLine) &&
         !boundaryChildWillBeMaterialized;
}

std::optional<uint64_t>
RefoldHeaderIncludeEditPlanner::SelectedArmBeginBoundaryByte(
    const HeaderInsertionPlanningState &state) const {
  auto rightArmRef = model_.FindArmRefAtPP(state.pos);
  if (!rightArmRef || !rightArmRef->group || !rightArmRef->arm)
    return std::nullopt;
  if (!rightArmRef->arm->selected || !rightArmRef->arm->span)
    return std::nullopt;
  if (rightArmRef->arm->span->begin != state.pos)
    return std::nullopt;
  if (!paths_.PathsEqual(rightArmRef->group->file, state.file))
    return std::nullopt;
  if (!rightArmRef->group->parentIncludeId ||
      *rightArmRef->group->parentIncludeId != state.include.id)
    return std::nullopt;

  auto leftArmRef = (state.pos > 0) ? model_.FindArmRefAtPP(state.pos - 1)
                                    : std::optional<RefoldModel::ArmRef>{};
  if (leftArmRef && leftArmRef->arm &&
      leftArmRef->arm->id == rightArmRef->arm->id)
    return std::nullopt;

  const bool explicitArmOwned =
      state.patch.condArm.present &&
      state.patch.condArm.armId == rightArmRef->arm->id;
  if (explicitArmOwned)
    return std::nullopt;

  return std::clamp<uint64_t>(rightArmRef->group->groupB, 0ULL, state.fileLen);
}

std::optional<RefoldHeaderIncludeEditPlanner::InsertAnchorCandidate>
RefoldHeaderIncludeEditPlanner::BuildSelectedConditionalBoundaryAnchor(
    const HeaderInsertionPlanningState &state) const {
  const std::optional<uint64_t> insertByte =
      SelectedArmBeginBoundaryByte(state);
  if (!insertByte || *insertByte > state.fileLen ||
      !AnchorMatchesCondArmCert(state, *insertByte))
    return std::nullopt;

  InsertAnchorCandidate candidate;
  candidate.path = AcceptedPathKind::IncludeInsertSelectedConditionalBoundary;
  candidate.anchorByte = *insertByte;
  candidate.witness.evidence =
      IncludeAnchorEvidenceKind::SelectedConditionalBoundary;
  candidate.witness.hasAnchorByte = true;
  candidate.witness.anchorByte = *insertByte;
  if (auto rightArmRef = model_.FindArmRefAtPP(state.pos);
      rightArmRef && rightArmRef->arm) {
    candidate.witness.hasCondArmId = true;
    candidate.witness.condArmId = rightArmRef->arm->id;
  }
  return candidate;
}

std::optional<RefoldHeaderIncludeEditPlanner::InsertAnchorCandidate>
RefoldHeaderIncludeEditPlanner::BuildChildBoundaryAnchor(
    const HeaderInsertionPlanningState &state) const {
  IncludeAnchorWitness childBoundaryWitness;
  const std::optional<uint64_t> insertByte = ComputeChildBoundaryInsertByte(
      state.patch, state.file, &childBoundaryWitness);
  if (!insertByte || *insertByte > state.fileLen ||
      !AnchorMatchesCondArmCert(state, *insertByte))
    return std::nullopt;

  InsertAnchorCandidate candidate;
  candidate.path = AcceptedPathKind::IncludeInsertChildBoundary;
  candidate.anchorByte = *insertByte;
  candidate.witness = childBoundaryWitness;
  candidate.witness.hasAnchorByte = true;
  candidate.witness.anchorByte = *insertByte;
  return candidate;
}

std::optional<uint64_t> RefoldHeaderIncludeEditPlanner::FindRightNeighborPP(
    StringRef file, uint64_t pos, uint64_t ppLo, uint64_t ppHi) const {
  const auto &tokmapByPP = model_.GetTokmapByPP();
  for (uint64_t pp = std::max(pos, ppLo); pp < ppHi; ++pp) {
    auto it = tokmapByPP.find(pp);
    if (it != tokmapByPP.end() && paths_.PathsEqual(it->second.file, file))
      return pp;
  }
  return std::nullopt;
}

std::optional<uint64_t> RefoldHeaderIncludeEditPlanner::FindLeftNeighborPP(
    StringRef file, uint64_t pos, uint64_t ppLo, uint64_t ppHi) const {
  if (pos <= ppLo || ppHi <= ppLo)
    return std::nullopt;

  const auto &tokmapByPP = model_.GetTokmapByPP();
  for (uint64_t pp = std::min(pos - 1, ppHi - 1);; --pp) {
    auto it = tokmapByPP.find(pp);
    if (it != tokmapByPP.end() && paths_.PathsEqual(it->second.file, file))
      return pp;
    if (pp == ppLo)
      break;
  }
  return std::nullopt;
}

bool RefoldHeaderIncludeEditPlanner::AppendRightNeighborAnchor(
    const HeaderInsertionPlanningState &state,
    SmallVectorImpl<InsertAnchorCandidate> &candidates) const {
  const std::optional<uint64_t> rightAnchorPP =
      FindRightNeighborPP(state.file, state.pos, state.ppLo, state.ppHi);
  if (!rightAnchorPP)
    return true;

  const std::optional<uint64_t> rightStartByte =
      sourceMapper_.ByteStartForPPInFile(state.file, *rightAnchorPP);
  IncludeAnchorWitness rightNeighborWitness;
  rightNeighborWitness.evidence = IncludeAnchorEvidenceKind::RightNeighborPP;
  rightNeighborWitness.hasNeighborPP = true;
  rightNeighborWitness.neighborPP = *rightAnchorPP;
  rightNeighborWitness.hasAnchorByte = rightStartByte.has_value();
  rightNeighborWitness.anchorByte = rightStartByte ? *rightStartByte : 0ULL;
  if (!rightStartByte) {
    state.plan.requiresIncludeRealization = true;
    state.plan.realizationReason =
        llvm::formatv("INSERT: failed to map anchorPP={0} in file {1}",
                      *rightAnchorPP, state.file)
            .str();
    return false;
  }

  if (!AnchorMatchesCondArmCert(state, *rightStartByte))
    return true;

  InsertAnchorCandidate candidate;
  candidate.path = AcceptedPathKind::IncludeInsertRightNeighborPP;
  candidate.anchorByte = *rightStartByte;
  candidate.witness = rightNeighborWitness;
  candidates.push_back(std::move(candidate));
  return true;
}

bool RefoldHeaderIncludeEditPlanner::AppendSecondaryInsertionAnchors(
    const HeaderInsertionPlanningState &state,
    SmallVectorImpl<InsertAnchorCandidate> &candidates) const {
  const std::optional<uint64_t> leftAnchorPP =
      FindLeftNeighborPP(state.file, state.pos, state.ppLo, state.ppHi);
  if (leftAnchorPP) {
    const std::optional<uint64_t> leftStartByte =
        sourceMapper_.ByteEndForPPInFile(state.file, *leftAnchorPP);
    const bool insertionAtIncludeEOF = state.pos >= state.ppHi;
    const std::optional<uint64_t> leftAnchorByte =
        leftStartByte
            ? std::optional<uint64_t>(insertionAtIncludeEOF ? state.fileLen
                                                            : *leftStartByte)
            : std::nullopt;
    IncludeAnchorWitness leftNeighborWitness;
    leftNeighborWitness.evidence = IncludeAnchorEvidenceKind::LeftNeighborPP;
    leftNeighborWitness.hasNeighborPP = true;
    leftNeighborWitness.neighborPP = *leftAnchorPP;
    leftNeighborWitness.hasAnchorByte = leftAnchorByte.has_value();
    leftNeighborWitness.anchorByte = leftAnchorByte ? *leftAnchorByte : 0ULL;
    if (!leftAnchorByte) {
      state.plan.requiresIncludeRealization = true;
      state.plan.realizationReason =
          llvm::formatv("INSERT: failed to map left-neighbor anchorPP={0} "
                        "in file {1}",
                        *leftAnchorPP, state.file)
              .str();
      return false;
    }

    if (AnchorMatchesCondArmCert(state, *leftAnchorByte)) {
      InsertAnchorCandidate candidate;
      candidate.path = AcceptedPathKind::IncludeInsertLeftNeighborPP;
      candidate.anchorByte = *leftAnchorByte;
      candidate.witness = leftNeighborWitness;
      candidates.push_back(std::move(candidate));
    }
    return true;
  }

  if (!state.decl)
    return true;

  const uint64_t declAnchorByte =
      std::clamp<uint64_t>(state.decl->headerE, 0ULL, state.fileLen);
  IncludeAnchorWitness declWitness;
  declWitness.evidence = IncludeAnchorEvidenceKind::DeclBoundary;
  declWitness.hasAnchorByte = true;
  declWitness.anchorByte = declAnchorByte;
  declWitness.hasDeclHeaderRange = true;
  declWitness.declHeaderB = state.decl->headerB;
  declWitness.declHeaderE = state.decl->headerE;
  if (AnchorMatchesCondArmCert(state, declAnchorByte)) {
    InsertAnchorCandidate candidate;
    candidate.path = AcceptedPathKind::IncludeInsertDeclBoundary;
    candidate.anchorByte = declAnchorByte;
    candidate.witness = declWitness;
    candidates.push_back(std::move(candidate));
  }
  return true;
}

bool RefoldHeaderIncludeEditPlanner::PlanPureInsertionPatch(
    const HeaderInsertionPlanningState &state) const {
  // First try the strongest insertion anchors: boundaries already tied to the
  // selected conditional arm or to a child include boundary.  The
  // child-boundary case is a declared include-preserving proof, not an
  // unclassified fallback branch: the witness names the direct child include
  // whose spelled directive supplies the anchor.
  SmallVector<InsertAnchorCandidate, 2> topTierCandidates;
  if (std::optional<InsertAnchorCandidate> candidate =
          BuildSelectedConditionalBoundaryAnchor(state))
    topTierCandidates.push_back(std::move(*candidate));
  if (std::optional<InsertAnchorCandidate> candidate =
          BuildChildBoundaryAnchor(state))
    topTierCandidates.push_back(std::move(*candidate));

  if (auto selected =
          SelectBestInsertCandidate(topTierCandidates, state.patch)) {
    CommitInsertCandidate(state, *selected);
    return true;
  }

  // If no structural boundary anchor won, look for the nearest mapped PP token
  // to the right in the same file and anchor before it.
  SmallVector<InsertAnchorCandidate, 1> rightCandidates;
  if (!AppendRightNeighborAnchor(state, rightCandidates))
    return false;
  if (auto selected = SelectBestInsertCandidate(rightCandidates, state.patch)) {
    CommitInsertCandidate(state, *selected);
    return true;
  }

  // Right-neighbor anchoring is preferred because it naturally inserts before
  // the next stable token. If unavailable, collect secondary anchors: the
  // nearest mapped left neighbor, or the containing declaration boundary when
  // no neighbor exists.
  SmallVector<InsertAnchorCandidate, 2> secondaryCandidates;
  if (!AppendSecondaryInsertionAnchors(state, secondaryCandidates))
    return false;
  if (auto selected =
          SelectBestInsertCandidate(secondaryCandidates, state.patch)) {
    CommitInsertCandidate(state, *selected);
    return true;
  }

  // No include-preserving insertion anchor discharged. Switch to the explicit
  // include-realization path instead of manufacturing a weaker witness.
  state.plan.requiresIncludeRealization = true;
  state.plan.realizationReason =
      llvm::formatv("INSERT: cannot anchor include patch in file {0} "
                    "(no admissible neighbors/decl/child boundary)",
                    state.file)
          .str();
  return false;
}

RefoldHeaderIncludeEditPlanner::IncludeTextEditPlan
RefoldHeaderIncludeEditPlanner::Compute(const IncludeEdits &ie,
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

  const HeaderSourceNeutralityContext headerSourceNeutrality =
      RefoldSourceNeutralityProof::BuildHeaderSourceNeutralityContext(
          model_, macroStateProof_, paths_, StringRef(headerText), file,
          ie.include->id, isWsOrCompleteCommentTrivia);

  // Header source-neutrality, source-envelope, macro-state carry, and
  // line-control predicates are named planner methods so proof obligations
  // remain inspectable.

  DenseSet<size_t> skippedIncludePatchIndices;

  for (size_t idx = 0; idx < ie.patches.size(); ++idx) {
    if (skippedIncludePatchIndices.count(idx))
      continue;

    IncludePatch coalescedPatch = ie.patches[idx];
    size_t skipThrough = idx;
    const IncludePatch *patchPtr = &ie.patches[idx];
    if (TryBuildConsumedHeaderConditionalCoalescedPatch(
            ie, file, StringRef(headerText), idx, coalescedPatch,
            skipThrough) ||
        TryBuildMaterialChildStateCoalescedPatch(ie, file, idx, coalescedPatch,
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

      TextEdit edit{p.directHeaderByteBegin,
                    p.directHeaderByteEnd,
                    p.insertBytes,
                    std::nullopt,
                    std::nullopt,
                    {},
                    {},
                    {}};
      textEditAssembler_.AttachAcceptedResultCarrier(
          edit,
          proofLattice_.AcceptedCandidateBuilder()
              .BuildAcceptedIncludeCandidate(
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

    if (isInsert) {
      // Normalize a duplicated boundary-token LCS tie across source-only #line
      // state before slicing and sideband-stripping the insertion payload.
      NormalizeDuplicatedBoundaryTokenAcrossLineControlGap(
          file, StringRef(headerText), ppHi, materialAStart, materialBStart,
          materialBEnd, materialInsertPos, materialInsertBytes);

      std::optional<std::pair<uint64_t, uint64_t>> bBytes =
          sourceMapper_.BTokenRangeToByteRange(materialBStart, materialBEnd);
      materialInsertBytes =
          textEditAssembler_.StripSeparatelyOwnedSidebandReplay(
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
              !paths_.PathsEqual(child.sitePath, file) ||
              !child.cover.IsValid())
            continue;
          if (child.cover.begin != materialAStart ||
              child.cover.end > materialAEnd)
            continue;

          const uint64_t childTokenCount = child.cover.end - child.cover.begin;
          if (materialBStart + childTokenCount > materialBEnd)
            continue;
          if (!tokenRangesHaveSameSpelling(aToks_, bToks_, child.cover.begin,
                                           child.cover.end, materialBStart,
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
      HeaderInsertionPlanningState insertionState{
          *ie.include,
          p,
          decl,
          plan,
          file,
          StringRef(headerText),
          StringRef(materialInsertBytes),
          fileLen,
          ppLo,
          ppHi,
          materialInsertPos};
      if (PlanPureInsertionPatch(insertionState))
        continue;
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
        if (it != tokmapByPP.end() &&
            paths_.PathsEqual(it->second.file, file)) {
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
      startByte = sourceMapper_.ByteStartForPPInFile(file, *firstPP);
      endByte = sourceMapper_.ByteEndForPPInFile(file, *lastPP);
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

      HeaderSourceEnvelopePlanningState sourceEnvelopeState{
          *ie.include,
          p,
          idx,
          decl,
          plan,
          file,
          StringRef(headerText),
          StringRef(materialInsertBytes),
          headerSourceNeutrality,
          ppLo,
          ppHi,
          coverBegin,
          coverEnd,
          materialAStart,
          materialAEnd,
          isDelete,
          startByte,
          endByte,
          mappedHeaderWitness,
          headerGapPreservations,
          headerSourceLineDirectiveResume,
          usedFullHeaderEnvelope};
      if (!TryApplyDeleteReplaceSourceEnvelope(sourceEnvelopeState))
        return plan;
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

    if (isReplace) {
      HeaderMacroStateCarryState carryState{
          *ie.include, plan,    file,        StringRef(headerText),
          startByte,   endByte, replacement, mappedHeaderWitness};
      TryCarryHeaderMacroStateAfterReplacement(carryState);
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
        std::string pieceText =
            PreservedGapPieceText(piece, StringRef(headerText));
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
        if (!HeaderLineDirectiveStartsAtPrefix(StringRef(headerText),
                                               *startByte))
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

      CheckHeaderSourceLineResumeStateTransitions(ie, idx, *endByte);
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
        edit,
        proofLattice_.AcceptedCandidateBuilder().BuildAcceptedIncludeCandidate(
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
      REFOLD_LOG_FATAL("include/apply",
                       "TextEdit out of bounds: bytes=[{0},{1}) size={2}",
                       e.start, e.end, headerText.size());
    }
  }

  return plan;
}

std::optional<uint64_t>
RefoldHeaderIncludeEditPlanner::ComputeChildBoundaryInsertByte(
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

  // Prefer anchoring before a child include whose PP cover begins at the gap.
  // The returned byte is the beginning of the child's spelled include
  // directive.
  if (!beginMatches.empty()) {
    if (beginMatches.size() != 1) {
      REFOLD_LOG_FATAL(
          "include/boundary",
          "ambiguous child include boundary: {0} children have cover.begin "
          "== {1} in file={2}; candidates: {3}",
          beginMatches.size(), pos, file,
          formatIncludeBoundaryCandidates(beginMatches));
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
      REFOLD_LOG_FATAL(
          "include/boundary",
          "ambiguous child include boundary: {0} children have cover.end == "
          "{1} in file={2}; candidates: {3}",
          endMatches.size(), pos, file,
          formatIncludeBoundaryCandidates(endMatches));
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

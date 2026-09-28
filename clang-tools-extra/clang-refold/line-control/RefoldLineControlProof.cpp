//===--- RefoldLineControlProof.cpp -----------------------------*- C++ -*-===//
//
// Proof-side line-control helpers for clang-refold.
//
// RefoldLineControlProof is a read-only service over producer line-control
// metadata, token maps, and macro topology.  It deliberately uses model-backed
// evidence for preserved `__LINE__` / `__FILE__` observers instead of guessing
// from final text.
//
//===----------------------------------------------------------------------===//

#include "line-control/RefoldLineControlProof.h"

#include "line-control/SourceLineDirectiveHelpers.h"
#include "macro/RefoldMacroTopology.h"
#include "model/RefoldPathIdentity.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldPreprocessingStructureIndexProvider.h"
#include "source/RefoldSourceGapProof.h"
#include "support/RefoldLog.h"
#include "support/StringUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <optional>
#include <string>

using namespace llvm;

namespace clang {
namespace refold {

const RefoldModel::MacroInvocation *
RefoldLineControlProof::LineStateObservableMacroSite(
    const RefoldModel::MacroInvocation &macro) const {
  const RefoldModel::MacroInvocation *site = &macro;
  const unsigned maxDepth =
      static_cast<unsigned>(model_.GetMacroInvocations().size());
  unsigned depth = 0;
  while (site->callerMacroId && depth++ < maxDepth) {
    const RefoldModel::MacroInvocation *caller =
        macroTopology_.FindMacroInvocationById(*site->callerMacroId);
    if (!caller)
      break;
    site = caller;
  }
  return site;
}

bool RefoldLineControlProof::SourcePrefixHasProducerActiveLineControl(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    uint64_t offset) const {
  // This query is used only to suppress an otherwise demanded synthetic
  // include-entry #line.  Only the directive class matters: an executed
  // directive of this owner that ends before `offset` is enough to know the
  // emitted child text will overwrite any synthetic entry #line before a later
  // observer can consume it.  A directive with no event either never executed
  // or predates the producer recording it; both keep the wrapper.
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls())
    if (event.ownerIncludeId == ownerIncludeId && event.active &&
        event.producerProven && event.siteE && *event.siteE <= offset &&
        paths_.PathsEqual(event.physicalFile, ownerFile))
      return true;
  return false;
}

bool RefoldLineControlProof::SourcePrefixNamesPresumedFile(
    StringRef ownerFile, uint64_t ownerIncludeId, uint64_t offset) const {
  const RefoldModel::IncludeItem *include =
      model_.GetIncludeById(ownerIncludeId);
  if (!include)
    return false;
  std::optional<std::string> presumedFile =
      paths_.ProducerEnteredFileSpelling(*include);
  if (!presumedFile)
    return false;

  SmallVector<const RefoldModel::LineControlEvent *, 4> events;
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls())
    if (event.ownerIncludeId == ownerIncludeId && event.active &&
        event.producerProven && event.siteB && event.siteE &&
        *event.siteE <= offset &&
        paths_.PathsEqual(event.physicalFile, ownerFile))
      events.push_back(&event);
  llvm::sort(events, [](const RefoldModel::LineControlEvent *lhs,
                        const RefoldModel::LineControlEvent *rhs) {
    return *lhs->siteB < *rhs->siteB;
  });

  // Once a directive names a file, every later one either names another or
  // keeps it, so the file stays independent of the includer from then on.
  bool named = false;
  for (const RefoldModel::LineControlEvent *event : events) {
    if (event->reason != RefoldModel::LineControlReason::Rename)
      return false;
    if (event->logicalFileAfter != *presumedFile)
      named = true;
    *presumedFile = event->logicalFileAfter.str();
  }
  return named;
}

LineStateObserverDemand
RefoldLineControlProof::IncludeSubtreeLineStateObserverDemand(
    uint64_t includeId) const {
  LineStateObserverDemand demand;

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesLine = macro.name == "__LINE__";
    // `__BASE_FILE__` is deliberately absent here, unlike in the owner-suffix
    // demand.  The directive this demand selects is the *include-entry* one,
    // and it names the entered child file.  Clang resolves `__BASE_FILE__` from
    // the top of the presumed include stack, so an entry directive can only
    // make the builtin read the child's own spelling -- never the top-level
    // file the producer observed.  It is therefore not a repair for this
    // observer at this boundary, and asking for one turns a completeness gap
    // into an unsatisfiable FileState obligation.  The observer is instead
    // discharged by `MaterializedHeaderRequiresBRealizationReason`, which
    // refuses to copy a preserved `__BASE_FILE__` spelling into a flattened
    // body at all, so no such spelling remains for an entry directive to serve.
    const bool observesFile = macro.name == "__FILE__";
    const bool observesFileName = macro.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      continue;

    // A builtin may be recorded at a macro definition site while the observable
    // use lives at an outer caller in this include subtree. Follow the same
    // caller chain used by the owner-suffix demand query so child-entry
    // wrappers are selected by the site that will remain in emitted source.
    const RefoldModel::MacroInvocation *site = &macro;
    unsigned depth = 0;
    const unsigned maxDepth =
        static_cast<unsigned>(model_.GetMacroInvocations().size());
    while (site->callerMacroId && depth++ < maxDepth) {
      const RefoldModel::MacroInvocation *caller =
          macroTopology_.FindMacroInvocationById(*site->callerMacroId);
      if (!caller)
        break;
      site = caller;
    }

    std::optional<uint64_t> owner = site->ownerIncludeId;
    if (!owner)
      owner = macro.ownerIncludeId;
    if (!owner || !model_.IncludeIsDescendantOrSelf(*owner, includeId))
      continue;

    if (!LineStateBuiltinInvocationIsPreservedObserver(macro)) {
      continue;
    }

    // The child's own directive ahead of the observer re-establishes the line
    // whatever the entry wrapper said, but the file only when it names one:
    // without a filename operand it keeps the presumed file, which after
    // inlining is the parent's rather than the child's.
    const bool lineReestablished = site->invFile && site->invB &&
                                   SourcePrefixHasProducerActiveLineControl(
                                       *site->invFile, owner, *site->invB);
    const bool fileReestablished =
        lineReestablished &&
        SourcePrefixNamesPresumedFile(*site->invFile, *owner, *site->invB);
    const bool needsLine = observesLine && !lineReestablished;
    const bool needsFile = observesFile && !fileReestablished;
    const bool needsFileName = observesFileName && !fileReestablished;
    if (!needsLine && !needsFile && !needsFileName)
      continue;

    demand.needsLine |= needsLine;
    demand.needsFile |= needsFile;
    demand.needsFileName |= needsFileName;
    demand.hasModelBackedLineStateDemand |=
        LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(macro);
    if (demand.needsLine && demand.needsFile && demand.needsFileName &&
        demand.hasModelBackedLineStateDemand)
      break;
  }

  return demand;
}

bool RefoldLineControlProof::IncludeEntryLineDirectiveDischargesLayoutBarrier(
    const RefoldModel::IncludeItem &child,
    StringRef parentOwnerFileForDemand) const {
  auto bufOrErr =
      MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(parentOwnerFileForDemand));
  if (!bufOrErr) {
    // Fail closed for the source-backed minimization proof: if the parent
    // owner cannot be read, do not infer that a layout barrier is unneeded.
    return true;
  }

  const MemoryBuffer &mb = **bufOrErr;
  StringRef parent(mb.getBufferStart(), mb.getBufferSize());
  const uint64_t includeSite =
      std::min<uint64_t>(child.siteB, static_cast<uint64_t>(parent.size()));
  if (includeSite == 0)
    return false;

  const auto &tokmapByPP = model_.GetTokmapByPP();

  auto ownerLineHasMappedToken = [&](uint64_t lineBegin,
                                     uint64_t lineEnd) -> bool {
    for (const auto &it : tokmapByPP) {
      const RefoldModel::TokMapEntry &tok = it.second;
      if (!paths_.PathsEqual(tok.file, parentOwnerFileForDemand))
        continue;
      if (tok.b >= lineBegin && tok.b < lineEnd)
        return true;
    }
    return false;
  };

  auto lineHasActiveIncludeSite = [&](uint64_t lineBegin,
                                      uint64_t lineEnd) -> bool {
    for (const RefoldModel::IncludeItem &inc : model_.GetIncludes()) {
      if (!paths_.PathsEqual(inc.sitePath, parentOwnerFileForDemand))
        continue;
      if (inc.siteB >= lineBegin && inc.siteB < lineEnd)
        return true;
    }
    return false;
  };

  auto lineCanEmitPreprocessedOutput = [&](StringRef line, uint64_t lineBegin,
                                           uint64_t lineEnd) -> bool {
    if (ownerLineHasMappedToken(lineBegin, lineEnd))
      return true;
    if (lineHasActiveIncludeSite(lineBegin, lineEnd))
      return true;

    // Pragmas are not part of the ordinary token map, but preserved active
    // pragmas are visible in `-E -P` output and therefore reset the physical
    // blank-line run. Other preprocessing directives are treated as zero-token
    // layout material for this proof; keeping a wrapper in those cases is
    // conservative and avoids inferring output from directive spelling alone.
    return stringutils::lineStartsWithDirectiveKeyword(line, "pragma");
  };

  bool sawOutputLine = false;
  bool sawZeroTokenLineBeforeFirstOutput = false;
  bool sawZeroTokenLineAfterLastOutput = false;

  uint64_t lineBegin = 0;
  while (lineBegin < includeSite) {
    uint64_t lineEnd = lineBegin;
    while (lineEnd < parent.size() && parent[lineEnd] != '\n')
      ++lineEnd;
    if (lineEnd >= includeSite)
      break; // Ignore indentation/trivia on the current include line.

    StringRef line = parent.slice(lineBegin, lineEnd);
    const bool canEmit =
        lineCanEmitPreprocessedOutput(line, lineBegin, lineEnd);
    if (canEmit) {
      sawOutputLine = true;
      sawZeroTokenLineAfterLastOutput = false;
    } else {
      if (sawOutputLine)
        sawZeroTokenLineAfterLastOutput = true;
      else
        sawZeroTokenLineBeforeFirstOutput = true;
    }

    lineBegin = lineEnd + 1;
  }

  const bool needsBarrier =
      (!sawOutputLine && sawZeroTokenLineBeforeFirstOutput) ||
      (sawOutputLine && sawZeroTokenLineAfterLastOutput);

  std::optional<std::string> childProducerSpelling =
      paths_.ProducerEnteredFileSpelling(child);
  if (needsBarrier && childProducerSpelling &&
      !childProducerSpelling->empty()) {
    std::optional<uint64_t> firstChildOutputByte;
    const auto &tokmapByPP = model_.GetTokmapByPP();
    for (const RefoldModel::PPArgSpan::PPSpan &span : child.spans) {
      if (!span.IsValid())
        continue;
      for (uint64_t pp = span.begin; pp < span.end; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it == tokmapByPP.end())
          continue;
        const RefoldModel::TokMapEntry &tok = it->second;
        if (!paths_.SamePhysicalIncludeFile(tok.file, child))
          continue;
        if (!firstChildOutputByte || tok.b < *firstChildOutputByte)
          firstChildOutputByte = tok.b;
      }
    }

    if (firstChildOutputByte &&
        SourcePrefixHasProducerActiveLineControl(
            *childProducerSpelling, child.id, *firstChildOutputByte)) {
      return false;
    }
  }

  return needsBarrier;
}

bool RefoldLineControlProof::SameLineControlPhysicalFile(StringRef lhs,
                                                         StringRef rhs) const {
  if (lhs == rhs)
    return true;

  auto isPseudoFile = [](StringRef path) {
    return path.starts_with("<") && path.ends_with(">");
  };
  if (isPseudoFile(lhs) || isPseudoFile(rhs))
    return false;

  return lineDirs_.ToAbsolutePath(lhs) == lineDirs_.ToAbsolutePath(rhs);
}

std::optional<uint64_t>
RefoldLineControlProof::LatestProducerLineControlEndBefore(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  std::optional<uint64_t> result;
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    if (!event.active || !event.producerProven || !event.siteE)
      continue;
    if (event.ownerIncludeId != ownerIncludeId)
      continue;
    if (!SameLineControlPhysicalFile(event.physicalFile, ownerFile))
      continue;
    if (*event.siteE > offset)
      continue;
    result = result ? std::max(*result, *event.siteE) : *event.siteE;
  }
  return result;
}

/// Return whether the producer skipped the owner occurrence's text at
/// \p introducer, so a directive spelled there was never executed.
///
/// A skipped range starts at the `#` of the conditional directive that began
/// skipping, so a directive inside the excluded group starts strictly after
/// it.
static bool lineControlIntroducerWasSkipped(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    uint64_t introducer) {
  for (const RefoldModel::SkippedRange &range : model.GetSkippedRanges())
    if (range.ownerIncludeId == ownerIncludeId && range.b < introducer &&
        introducer < range.e && paths.PathsEqual(range.physicalFile, ownerFile))
      return true;
  return false;
}

LineDirectiveLocation RefoldLineControlProof::OwnerLineStateAt(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    StringRef ownerBytes, uint64_t offset, StringRef defaultFile) const {
  offset = std::min<uint64_t>(offset, ownerBytes.size());

  bool proven = true;
  std::optional<uint64_t> unprovenAt;
  auto markUnproven = [&](std::optional<uint64_t> at) {
    proven = false;
    if (at && (!unprovenAt || *at > *unprovenAt))
      unprovenAt = at;
  };

  // The owner's events that start before `offset`, and the latest of them
  // whose effect has begun there.
  size_t ownerEventsBefore = 0;
  const RefoldModel::LineControlEvent *latest = nullptr;
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    if (event.ownerIncludeId != ownerIncludeId ||
        !paths_.PathsEqual(event.physicalFile, ownerFile))
      continue;
    if (!event.siteB || !event.siteE) {
      // Where this directive sits is unknown, so it may precede `offset`.
      markUnproven(std::nullopt);
      continue;
    }
    if (*event.siteB >= offset)
      continue;
    ++ownerEventsBefore;
    if (!event.active || !event.producerProven)
      markUnproven(*event.siteB);
    if (*event.siteE <= offset && (!latest || *event.siteE > *latest->siteE))
      latest = &event;
  }

  // Every lexical line-control directive ahead of `offset` must be one of
  // those events, or text Clang skipped; anything else was executed without a
  // record, or its record could not be placed.
  const RefoldPreprocessingStructureIndex *index =
      structureIndexes_
          ? structureIndexes_->Get(ownerFile, ownerIncludeId).index
          : nullptr;
  if (!index || index->GetSourceSize() != ownerBytes.size()) {
    markUnproven(std::nullopt);
  } else {
    size_t boundBefore = 0;
    for (const PreprocessingStructureInterval *interval :
         index->FindOverlapping(0, offset)) {
      if (interval->kind != PreprocessingStructureKind::LineControl ||
          interval->structureSpellingBegin >= offset)
        continue;
      if (interval->end > offset) {
        markUnproven(interval->structureSpellingBegin);
        continue;
      }
      if (interval->modelKind ==
              PreprocessingStructureModelKind::LineControlEvent &&
          interval->IsProducerBound()) {
        ++boundBefore;
        continue;
      }
      if (!lineControlIntroducerWasSkipped(model_, paths_, ownerFile,
                                           ownerIncludeId,
                                           interval->structureSpellingBegin))
        markUnproven(interval->structureSpellingBegin);
    }
    // An event the index could not bind has a site no lexical directive
    // matches, so the count exposes it even though no interval names it.
    if (boundBefore != ownerEventsBefore)
      markUnproven(std::nullopt);
  }

  if (!latest)
    return LineDirectiveLocation(
        defaultFile,
        1 + stringutils::countPhysicalLineBreaks(ownerBytes, 0, offset), proven,
        unprovenAt);
  return LineDirectiveLocation(latest->logicalFileAfter,
                               latest->logicalLineAfter +
                                   stringutils::countPhysicalLineBreaks(
                                       ownerBytes, *latest->siteE, offset),
                               proven, unprovenAt);
}

std::optional<uint64_t>
RefoldLineControlProof::LineControlDirectiveLineStartContaining(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    uint64_t offset) const {
  const RefoldPreprocessingStructureIndex *index =
      structureIndexes_
          ? structureIndexes_->Get(ownerFile, ownerIncludeId).index
          : nullptr;
  if (!index)
    return std::nullopt;
  for (const PreprocessingStructureInterval *interval :
       index->FindOverlapping(offset, offset + 1))
    if (interval->kind == PreprocessingStructureKind::LineControl &&
        interval->structureSpellingBegin < offset && offset < interval->end)
      return interval->begin;
  return std::nullopt;
}

/// Return the line-marker flags that make a resume directive establish
/// \p kind, or nullopt for an event whose map predates recording file kinds.
static std::optional<StringRef>
lineMarkerKindFlags(std::optional<RefoldModel::LineControlFileKind> kind) {
  if (!kind)
    return std::nullopt;
  switch (*kind) {
  case RefoldModel::LineControlFileKind::User:
    return StringRef();
  case RefoldModel::LineControlFileKind::System:
    return StringRef("3");
  case RefoldModel::LineControlFileKind::ExternCSystem:
    return StringRef("3 4");
  }
  return std::nullopt;
}

std::optional<SourceLineDirectiveGapResume>
RefoldLineControlProof::LineControlGapResume(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    StringRef ownerBytes, uint64_t gapBegin, uint64_t gapEnd,
    uint64_t resumeOffset, SmallVectorImpl<uint64_t> *operandMacroIds) const {
  if (gapBegin >= gapEnd || gapEnd > resumeOffset ||
      resumeOffset > ownerBytes.size() || !structureIndexes_)
    return std::nullopt;
  const RefoldPreprocessingStructureIndex *index =
      structureIndexes_->Get(ownerFile, ownerIncludeId).index;
  if (!index || index->GetSourceSize() != ownerBytes.size())
    return std::nullopt;

  // The gap's directives, each an executed line-control directive of this
  // owner; the shared gap theorem then proves every other byte is trivia.
  SmallVector<SourceGapProofPiece, 4> pieces;
  SmallVector<const RefoldModel::LineControlEvent *, 4> gapEvents;
  for (const PreprocessingStructureInterval *interval :
       index->FindOverlapping(gapBegin, gapEnd)) {
    if (interval->kind != PreprocessingStructureKind::LineControl ||
        interval->modelKind !=
            PreprocessingStructureModelKind::LineControlEvent ||
        !interval->IsProducerBound())
      return std::nullopt;
    const auto event = llvm::find_if(
        model_.GetLineControls(), [&](const RefoldModel::LineControlEvent &e) {
          return e.id == *interval->modelItemId;
        });
    if (event == model_.GetLineControls().end())
      return std::nullopt;
    gapEvents.push_back(&*event);
    pieces.push_back(SourceGapProofPiece{
        interval->begin, interval->end, event->id,
        static_cast<uint32_t>(PreprocessingStructureKind::LineControl),
        /*nestingClass=*/0, /*absorbedNestingClasses=*/0, pieces.size()});
  }
  if (pieces.empty() ||
      !proveSourceGapWithIndexedTrivia(*index, gapBegin, gapEnd, pieces))
    return std::nullopt;

  const LineDirectiveLocation state = OwnerLineStateAt(
      ownerFile, ownerIncludeId, ownerBytes, resumeOffset, ownerFile);
  if (!state.producerProven)
    return std::nullopt;

  // What the directives from the gap on did to the presumed include stack,
  // and the file kind in effect at the resume.
  size_t entered = 0;
  const RefoldModel::LineControlEvent *latest = nullptr;
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    if (event.ownerIncludeId != ownerIncludeId || !event.siteB ||
        !event.siteE || *event.siteE > resumeOffset ||
        !paths_.PathsEqual(event.physicalFile, ownerFile))
      continue;
    if (!latest || *event.siteE > *latest->siteE)
      latest = &event;
    if (*event.siteB < gapBegin)
      continue;
    if (event.reason == RefoldModel::LineControlReason::Exit)
      return std::nullopt;
    if (event.reason == RefoldModel::LineControlReason::Enter)
      ++entered;
  }
  std::optional<StringRef> kindFlags =
      latest ? lineMarkerKindFlags(latest->fileKind) : std::nullopt;
  if (entered > 1 || !kindFlags)
    return std::nullopt;

  SourceLineDirectiveGapResume resume;
  resume.lineAtResume = state.lineNo;
  resume.fileSpelling = state.fileSpelling;
  if (entered)
    resume.lineMarkerFlags = "1";
  if (!kindFlags->empty()) {
    if (!resume.lineMarkerFlags.empty())
      resume.lineMarkerFlags += ' ';
    resume.lineMarkerFlags += *kindFlags;
  }

  // The macro invocations the gap's directives evaluated: those spelled in a
  // directive, closed over the recorded caller edges of their expansions.
  SmallVector<uint64_t, 8> operandIds;
  for (const RefoldModel::MacroInvocation &macro : model_.GetMacroInvocations())
    if (macro.ownerIncludeId == ownerIncludeId && macro.invFile && macro.invB &&
        macro.invE && paths_.PathsEqual(*macro.invFile, ownerFile) &&
        llvm::any_of(gapEvents, [&](const RefoldModel::LineControlEvent *e) {
          return *e->siteB <= *macro.invB && *macro.invE <= *e->siteE;
        }))
      operandIds.push_back(macro.id);
  for (bool grew = !operandIds.empty(); grew;) {
    grew = false;
    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations())
      if (macro.callerMacroId &&
          llvm::is_contained(operandIds, *macro.callerMacroId) &&
          !llvm::is_contained(operandIds, macro.id)) {
        operandIds.push_back(macro.id);
        grew = true;
      }
  }

  // A `__DATE__`, `__TIME__` or `__TIMESTAMP__` operand names a file that
  // depends on when A was produced, and replaying the recorded name would bake
  // that moment into the output.  It is admitted only when the suffix cannot
  // observe the presumed file, which makes any stable spelling
  // token-equivalent; the one in effect before the gap is kept.
  const bool clockDependentFile = llvm::any_of(operandIds, [&](uint64_t id) {
    const RefoldModel::MacroInvocation *macro =
        macroTopology_.FindMacroInvocationById(id);
    return macro && (macro->name == "__DATE__" || macro->name == "__TIME__" ||
                     macro->name == "__TIMESTAMP__");
  });
  if (clockDependentFile) {
    if (sourceSuffixMayObservePresumedFileSpelling(
            model_, ownerFile, resumeOffset, paths_, ownerBytes))
      return std::nullopt;
    resume.fileSpelling = OwnerLineStateAt(ownerFile, ownerIncludeId,
                                           ownerBytes, gapBegin, ownerFile)
                              .fileSpelling;
  }

  if (operandMacroIds)
    operandMacroIds->append(operandIds.begin(), operandIds.end());
  return resume;
}

bool RefoldLineControlProof::LineStateBuiltinInvocationIsPreservedObserver(
    const RefoldModel::MacroInvocation &macro) const {
  // The demand side of synthetic #line insertion is intentionally about
  // *preserved observers*, not merely about source spellings that existed in
  // the original owner. A later accepted macro-realization edit may already
  // materialize a changed __LINE__ value as an ordinary literal. In that case
  // inserting a #line before the materialized literal is gratuitous and can
  // make the refolding less faithful to the B-side edit.
  //
  // The producer/LCS token map gives us a deterministic proof for the only case
  // that should demand resync here: the builtin's realized A-side token surface
  // survived token-identically into B. If any produced token is unmapped, maps
  // back to a different A token, or has different spelling in B, the builtin is
  // not considered a preserved observer for line-resync demand; the ordinary
  // macro realization machinery is then responsible for materializing it.
  if (!macro.cover.IsValid())
    return true;

  if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
    return true;

  int64_t previousB = -1;
  for (uint64_t aTok = macro.cover.begin; aTok < macro.cover.end; ++aTok) {
    if (aTok >= static_cast<uint64_t>(abTokMapA2B_.size()))
      return false;

    const int64_t bTok = abTokMapA2B_[static_cast<size_t>(aTok)];
    if (bTok < 0)
      return false;
    if (bTok >= static_cast<int64_t>(bToks_.size()) ||
        aTok >= static_cast<uint64_t>(aToks_.size()))
      return false;
    if (static_cast<uint64_t>(bTok) >= abTokMapB2A_.size())
      return false;
    if (abTokMapB2A_[static_cast<size_t>(bTok)] != static_cast<int64_t>(aTok))
      return false;

    if (previousB >= 0 && bTok != previousB + 1)
      return false;
    previousB = bTok;

    if (aToks_[static_cast<size_t>(aTok)].spelling !=
        bToks_[static_cast<size_t>(bTok)].spelling)
      return false;
  }

  return true;
}

bool RefoldLineControlProof::
    LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(
        const RefoldModel::MacroInvocation &macro) const {
  // Direct lexical builtin tokens that survive into ordinary emitted source are
  // visible without extra model facts.  If a predefined builtin is
  // reached through a caller macro, the final source normally contains the
  // caller spelling rather than the builtin token; record that the demand needs
  // model-backed line-state evidence instead of lexical discovery alone.
  if (macro.callerMacroId)
    return true;

  if (!macro.invFile || !macro.invB)
    return true;

  // Missing cover/map evidence is already treated as preserved by
  // LineStateBuiltinInvocationIsPreservedObserver() for emission safety.  It
  // also requires model-backed line-state demand rather than direct lexical
  // discovery.
  if (!macro.cover.IsValid())
    return true;
  if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
    return true;

  return false;
}

LineStateObserverDemand
RefoldLineControlProof::OwnerSuffixLineStateObserverDemand(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  LineStateObserverDemand demand;

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesLine = macro.name == "__LINE__";
    const bool observesFile =
        macro.name == "__FILE__" || macro.name == "__BASE_FILE__";
    const bool observesFileName = macro.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      continue;
    if (macro.ownerIncludeId != ownerIncludeId)
      continue;

    // The producer records nested predefined builtins at their definition-site
    // spelling when they are expanded from another macro replacement list. For
    // example, RF_MARK(x) may contain __LINE__ in common.h even though the
    // observable line-state use is the RF_MARK(...) invocation in the current
    // header.  Line-resync decisions therefore have to project a recorded
    // builtin use back to its outermost caller before comparing byte offsets in
    // the owner file.  This is still a deterministic map-backed proof: every
    // step follows caller_macro_id edges emitted by the producer.
    const RefoldModel::MacroInvocation *site = &macro;
    unsigned depth = 0;
    const unsigned maxDepth =
        static_cast<unsigned>(model_.GetMacroInvocations().size());
    while (site->callerMacroId && depth++ < maxDepth) {
      const RefoldModel::MacroInvocation *caller =
          macroTopology_.FindMacroInvocationById(*site->callerMacroId);
      if (!caller)
        break;
      site = caller;
    }

    if (!site->invFile || !site->invB)
      continue;
    if (!paths_.PathsEqual(*site->invFile, ownerFile))
      continue;
    if (*site->invB < offset)
      continue;

    if (!LineStateBuiltinInvocationIsPreservedObserver(macro)) {
      continue;
    }

    demand.needsLine |= observesLine;
    demand.needsFile |= observesFile;
    demand.needsFileName |= observesFileName;
    demand.hasModelBackedLineStateDemand |=
        LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(macro);
    if (demand.needsLine && demand.needsFile && demand.needsFileName &&
        demand.hasModelBackedLineStateDemand)
      break;
  }

  return demand;
}

std::optional<LineStateObserverSite>
RefoldLineControlProof::FirstOwnerSuffixLineStateObserverSite(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  std::optional<LineStateObserverSite> best;

  auto considerSite = [&](const RefoldModel::MacroInvocation &builtin,
                          const RefoldModel::MacroInvocation &site) {
    const bool observesLine = builtin.name == "__LINE__";
    const bool observesFile =
        builtin.name == "__FILE__" || builtin.name == "__BASE_FILE__";
    const bool observesFileName = builtin.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      return;
    if (site.ownerIncludeId != ownerIncludeId)
      return;
    if (!site.invFile || !site.invB)
      return;
    if (!paths_.PathsEqual(*site.invFile, ownerFile))
      return;
    if (*site.invB < offset)
      return;
    if (macroTopology_.IsInvocationInsideDefineDirective(site))
      return;
    if (!LineStateBuiltinInvocationIsPreservedObserver(builtin))
      return;

    if (!best || *site.invB < best->offset) {
      best = LineStateObserverSite{};
      best->offset = *site.invB;
    }
    best->demand.needsLine |= observesLine;
    best->demand.needsFile |= observesFile;
    best->demand.needsFileName |= observesFileName;
    best->demand.hasModelBackedLineStateDemand |=
        LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(builtin);
  };

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesLine = macro.name == "__LINE__";
    const bool observesFile =
        macro.name == "__FILE__" || macro.name == "__BASE_FILE__";
    const bool observesFileName = macro.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      continue;

    const RefoldModel::MacroInvocation *site = &macro;
    unsigned depth = 0;
    const unsigned maxDepth =
        static_cast<unsigned>(model_.GetMacroInvocations().size());
    while (site && depth++ <= maxDepth) {
      considerSite(macro, *site);
      if (!site->callerMacroId)
        break;
      site = macroTopology_.FindMacroInvocationById(*site->callerMacroId);
    }
  }

  return best;
}

bool RefoldLineControlProof::OwnerSuffixHasLineStateSensitiveBuiltin(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  return OwnerSuffixLineStateObserverDemand(ownerIncludeId, ownerFile, offset)
      .Any();
}

std::optional<uint64_t>
RefoldLineControlProof::AdvanceInsertionAnchorPastSourceLineControlPrefix(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    StringRef ownerBytes, uint64_t anchor) const {
  if (anchor > ownerBytes.size())
    return std::nullopt;

  uint64_t cur = anchor;
  bool advanced = false;

  while (true) {
    const RefoldModel::LineControlEvent *best = nullptr;

    for (const RefoldModel::LineControlEvent &event :
         model_.GetLineControls()) {
      if (!event.active || !event.producerProven || !event.siteB ||
          !event.siteE)
        continue;
      if (event.ownerIncludeId != ownerIncludeId)
        continue;
      if (!paths_.PathsEqual(event.physicalFile, ownerFile))
        continue;
      if (*event.siteB < cur || *event.siteE <= cur ||
          *event.siteE > ownerBytes.size())
        continue;

      // Only slide across zero-token whitespace before the directive.  Comments
      // or other trivia may be intentionally positioned before the directive
      // and must not be silently crossed by this source-placement rule.
      if (!stringutils::isWs(ownerBytes.slice(cur, *event.siteB)))
        continue;

      if (!best || *event.siteB < *best->siteB ||
          (*event.siteB == *best->siteB && event.id < best->id))
        best = &event;
    }

    if (!best)
      break;

    cur = *best->siteE;
    advanced = true;
  }

  if (!advanced || cur == anchor)
    return std::nullopt;

  return cur;
}

bool RefoldLineControlProof::TUInsertionCanDeferResyncToConditionalJoin(
    bool advancedOverSourceLineControlPrefix, StringRef tuPath,
    uint64_t anchor) const {
  if (!advancedOverSourceLineControlPrefix)
    return false;

  std::optional<LineStateObserverSite> firstObserver =
      FirstOwnerSuffixLineStateObserverSite(std::nullopt, tuPath, anchor);
  if (!firstObserver || !firstObserver->demand.needsLine)
    return false;

  const RefoldModel::CondGroup *innermost = nullptr;
  for (const RefoldModel::CondGroup *group :
       model_.GetCondGroups(tuPath, std::nullopt)) {
    if (!group || !paths_.PathsEqual(group->file, tuPath) ||
        group->parentIncludeId || !group->ContainsByte(anchor)) {
      continue;
    }
    if (!innermost || (group->groupB >= innermost->groupB &&
                       group->groupE <= innermost->groupE)) {
      innermost = group;
    }
  }

  if (!innermost || firstObserver->offset < innermost->groupE)
    return false;

  return true;
}

} // namespace refold
} // namespace clang

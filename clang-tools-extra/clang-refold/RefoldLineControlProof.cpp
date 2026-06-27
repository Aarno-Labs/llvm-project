//===--- RefoldLineControlProof.cpp ---------------------------*- C++ -*-===//
//
// Proof-side line-control helpers for clang-refold.
//
// The routines in this file are still RefoldEngine members because they depend
// on the engine's immutable model/source indexes.  Keeping the definitions in a
// dedicated translation unit separates line-state proof and audit decisions
// from materialization and text-emission code without changing call sites or
// proof policy.
//
//===----------------------------------------------------------------------===//

#include "RefoldLog.h"
#include "RefoldEngine.h"
#include "RefoldLineControlProof.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cctype>
#include <optional>

using namespace llvm;

namespace clang {
namespace refold {

bool RefoldEngine::AuditFinalLineControlAuthorityContract(
    const FinalLineControlAuthorityContract &authority, StringRef role) const {
  if (!IsNoLegacyAuditEnabled())
    return true;

  auto report = [&](StringRef detail) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::FinalLineControlLivenessWithoutObligation, role,
        detail));
  };

  if (!authority.compactRemovalProofIsAuthoritative)
    report("final line-control compact removal proof is not authoritative");
  if (!authority.fixedPointPruningIsAuthoritative)
    report("final line-control fixed-point pruning is not authoritative");
  if (!authority.validationCallbackIsAuthoritative)
    report("final line-control validation callback is not authoritative");

  return authority.IsClosedUnderCompactProofs();
}

bool RefoldEngine::AuditFinalLineControlRemovalProofPopulation(
    ArrayRef<FinalLineControlPruneCandidate> candidates, StringRef role) const {
  if (!IsNoLegacyAuditEnabled())
    return true;

  size_t missing = 0;
  for (const FinalLineControlPruneCandidate &candidate : candidates)
    if (!HasCompleteFinalLineControlProof(candidate))
      ++missing;

  if (missing == 0)
    return true;

  ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
      LegacyPathKind::FinalLineControlLivenessWithoutObligation, role,
      llvm::formatv(
          "{0} final line-control prune candidate(s) lack compact "
          "obligation/removal proof; generation sites must populate both facts "
          "before compact final-line-control pruning may run",
          missing)
          .str()));
  return false;
}
const RefoldModel::MacroInvocation *
RefoldEngine::LineStateObservableMacroSite(
    const RefoldModel::MacroInvocation &macro) const {
  const RefoldModel::MacroInvocation *site = &macro;
  const unsigned maxDepth =
      static_cast<unsigned>(model_.GetMacroInvocations().size());
  unsigned depth = 0;
  while (site->callerMacroId && depth++ < maxDepth) {
    const RefoldModel::MacroInvocation *caller =
        FindMacroInvocationById(*site->callerMacroId);
    if (!caller)
      break;
    site = caller;
  }
  return site;
}

/// Return true iff a physical source line is lexically a line-control
/// directive.  This intentionally recognizes only the directive class, not the
/// expanded operands: for entry-wrapper minimization it is enough to know that
/// the emitted child text will overwrite any synthetic entry #line before a
/// later observer can consume it.
static bool refoldLineSpellingIsLineControlDirective(StringRef line) {
  const size_t to = line.size();
  size_t p = 0;
  while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
    ++p;
  if (p >= to || line[p] != '#')
    return false;
  ++p;
  while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
    ++p;

  if (p + 4 <= to && line.substr(p, 4) == "line" &&
      (p + 4 == to || stringutils::isWs(line[p + 4])))
    return true;

  // Clang/GCC numeric line-control spelling: # 123 "file".
  return p < to && std::isdigit(static_cast<unsigned char>(line[p]));
}

/// Return the original-source byte offset of the directive-introducing '#'
/// for a physical source line.  The conditional model is expressed in original
/// byte coordinates, so the activity query below must use the real source byte
/// rather than the trimmed line-local position.
static bool refoldIsHorizontalWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r';
}

static std::optional<uint64_t>
refoldFindDirectiveHashOffset(StringRef src, uint64_t lineBegin,
                              uint64_t lineEnd) {
  uint64_t p = lineBegin;
  while (p < lineEnd && p < src.size()) {
    if (refoldIsHorizontalWhitespace(src[p])) {
      ++p;
      continue;
    }

    if (src[p] == '\\') {
      if (p + 1 < lineEnd && src[p + 1] == '\n') {
        p += 2;
        continue;
      }
      if (p + 2 < lineEnd && src[p + 1] == '\r' && src[p + 2] == '\n') {
        p += 3;
        continue;
      }
    }

    if (p + 1 < lineEnd && src[p] == '/' && src[p + 1] == '*') {
      p += 2;
      bool closed = false;
      while (p + 1 < lineEnd) {
        if (src[p] == '\\') {
          if (p + 1 < lineEnd && src[p + 1] == '\n') {
            p += 2;
            continue;
          }
          if (p + 2 < lineEnd && src[p + 1] == '\r' &&
              src[p + 2] == '\n') {
            p += 3;
            continue;
          }
        }
        if (src[p] == '*' && src[p + 1] == '/') {
          p += 2;
          closed = true;
          break;
        }
        ++p;
      }
      if (!closed)
        return std::nullopt;
      continue;
    }

    if (p + 1 < lineEnd && src[p] == '/' && src[p + 1] == '/')
      return std::nullopt;

    if (src[p] == '#')
      return p;

    return std::nullopt;
  }
  return std::nullopt;
}

bool RefoldEngine::SourcePrefixHasProducerActiveLineControl(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    uint64_t offset) const {
  auto bufOrErr =
      MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerFile));
  if (!bufOrErr) {
    // This query is used only to suppress an otherwise demanded synthetic
    // include-entry #line.  If the owner source cannot be read, fail closed and
    // keep the wrapper rather than inferring that the real line-control stream
    // will kill it.
    return false;
  }

  const MemoryBuffer &mb = **bufOrErr;
  StringRef src(mb.getBufferStart(), mb.getBufferSize());
  const uint64_t limit = std::min<uint64_t>(offset, src.size());

  auto directiveIsProducerActive = [&](uint64_t hashOffset) -> bool {
    for (const RefoldModel::CondGroup &group : model_.GetConds()) {
      if (!PathsEqual(group.file, ownerFile) ||
          group.parentIncludeId != ownerIncludeId)
        continue;
      if (!group.ContainsByte(hashOffset))
        continue;

      bool sawSelectedArm = false;
      bool selectedArmContainsDirective = false;
      for (const RefoldModel::CondArm &arm : group.arms) {
        if (!arm.selected)
          continue;
        sawSelectedArm = true;
        if (arm.ContainsByte(hashOffset)) {
          selectedArmContainsDirective = true;
          break;
        }
      }

      if (selectedArmContainsDirective)
        continue;

      // If a selected material-producing arm exists but does not contain the
      // directive, the directive is proven inactive.  If no selected arm
      // exists, the active branch may have produced only directive effects,
      // which is unproven from CondArm::selected alone; keep the synthetic
      // wrapper in that case as well.
      (void)sawSelectedArm;
      return false;
    }
    return true;
  };

  uint64_t lineBegin = 0;
  while (lineBegin < limit) {
    uint64_t lineEnd = lineBegin;
    while (lineEnd < src.size() && src[lineEnd] != '\n')
      ++lineEnd;

    const uint64_t clampedEnd = std::min<uint64_t>(lineEnd, limit);
    if (std::optional<uint64_t> hash =
            refoldFindDirectiveHashOffset(src, lineBegin, clampedEnd)) {
      if (refoldLineSpellingIsLineControlDirective(
              src.slice(*hash, clampedEnd)) &&
          directiveIsProducerActive(*hash)) {
        return true;
      }
    }

    if (lineEnd >= limit || lineEnd >= src.size())
      break;
    lineBegin = lineEnd + 1;
  }

  return false;
}

RefoldEngine::LineStateObserverDemand
RefoldEngine::IncludeSubtreeLineStateObserverDemand(uint64_t includeId) const {
  LineStateObserverDemand demand;

  auto isDescendantOrSelf = [&](uint64_t owner) -> bool {
    uint64_t cur = owner;
    while (true) {
      if (cur == includeId)
        return true;
      const RefoldModel::IncludeItem *inc = model_.GetIncludeById(cur);
      if (!inc || !inc->parent)
        return false;
      cur = *inc->parent;
    }
  };

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesLine = macro.name == "__LINE__";
    const bool observesFile = macro.name == "__FILE__" ||
                              macro.name == "__BASE_FILE__";
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
          FindMacroInvocationById(*site->callerMacroId);
      if (!caller)
        break;
      site = caller;
    }

    std::optional<uint64_t> owner = site->ownerIncludeId;
    if (!owner)
      owner = macro.ownerIncludeId;
    if (!owner || !isDescendantOrSelf(*owner))
      continue;

    if (!LineStateBuiltinInvocationIsPreservedObserver(macro)) {
      continue;
    }

    if (site->invFile && site->invB &&
        SourcePrefixHasProducerActiveLineControl(*site->invFile, owner,
                                                 *site->invB)) {
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

bool RefoldEngine::IncludeSubtreeHasLineStateSensitiveBuiltin(
    uint64_t includeId) const {
  return IncludeSubtreeLineStateObserverDemand(includeId).Any();
}

bool RefoldEngine::IncludeEntryLineDirectiveDischargesLayoutBarrier(
    const RefoldModel::IncludeItem &child,
    StringRef parentOwnerFileForDemand) const {
  auto bufOrErr =
      MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(parentOwnerFileForDemand));
  if (!bufOrErr) {
    // Failing closed here preserves the old wrapper behavior. The minimization
    // proof is source-backed; if the parent owner cannot be read, do not infer
    // that a layout barrier is unneeded.
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
      if (!PathsEqual(tok.file, parentOwnerFileForDemand))
        continue;
      if (tok.b >= lineBegin && tok.b < lineEnd)
        return true;
    }
    return false;
  };

  auto lineHasActiveIncludeSite = [&](uint64_t lineBegin,
                                      uint64_t lineEnd) -> bool {
    for (const RefoldModel::IncludeItem &inc : model_.GetIncludes()) {
      if (!PathsEqual(inc.sitePath, parentOwnerFileForDemand))
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

  StringRef childProducerSpelling = producerEnteredFileSpelling(child);
  if (needsBarrier && !childProducerSpelling.empty()) {
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
        if (!samePhysicalIncludeFile(tok.file, child))
          continue;
        if (!firstChildOutputByte || tok.b < *firstChildOutputByte)
          firstChildOutputByte = tok.b;
      }
    }

    if (firstChildOutputByte &&
        SourcePrefixHasProducerActiveLineControl(childProducerSpelling,
                                                 child.id,
                                                 *firstChildOutputByte)) {
      return false;
    }
  }

  return needsBarrier;
}

bool RefoldEngine::SameLineControlPhysicalFile(StringRef lhs,
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

std::optional<uint64_t> RefoldEngine::LatestProducerLineControlEndBefore(
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

std::optional<LineDirectiveLocation>
RefoldEngine::ProducerBackedLineControlLocationAt(
    StringRef ownerBytes, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, uint64_t eventEndLimit,
    uint64_t locationOffset) const {
  const RefoldModel::LineControlEvent *best = nullptr;
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    if (!event.active || !event.producerProven || !event.siteE)
      continue;
    if (event.ownerIncludeId != ownerIncludeId)
      continue;
    if (!SameLineControlPhysicalFile(event.physicalFile, ownerFile))
      continue;
    if (*event.siteE > eventEndLimit)
      continue;
    if (!best || *event.siteE > *best->siteE ||
        (*event.siteE == *best->siteE && event.id > best->id))
      best = &event;
  }

  if (!best)
    return std::nullopt;

  const size_t delta = stringutils::countNonSplicedNewlines(
      ownerBytes, *best->siteE, locationOffset);
  return LineDirectiveLocation(best->logicalFileAfter,
                               best->logicalLineAfter + delta, true);
}

LineDirectiveLocation RefoldEngine::LogicalLocationAtOwnerOffset(
    StringRef ownerBytes, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, uint64_t offset) const {
  LineDirectiveLocation loc = LineDirectiveInserter::LogicalLocationAtOffset(
      ownerBytes, static_cast<size_t>(offset), ownerFile, model_, ownerFile,
      ownerIncludeId);
  if (loc.producerProven)
    return loc;
  if (std::optional<LineDirectiveLocation> producerLoc =
          ProducerBackedLineControlLocationAt(ownerBytes, ownerFile,
                                              ownerIncludeId, offset, offset))
    return *producerLoc;
  return loc;
}

bool RefoldEngine::LineStateBuiltinInvocationIsPreservedObserver(
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
    if (abTokMapB2A_[static_cast<size_t>(bTok)] !=
        static_cast<int64_t>(aTok))
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

bool RefoldEngine::LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(
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

RefoldEngine::LineStateObserverDemand
RefoldEngine::OwnerSuffixLineStateObserverDemand(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  LineStateObserverDemand demand;

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesLine = macro.name == "__LINE__";
    const bool observesFile = macro.name == "__FILE__" ||
                              macro.name == "__BASE_FILE__";
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
          FindMacroInvocationById(*site->callerMacroId);
      if (!caller)
        break;
      site = caller;
    }

    if (!site->invFile || !site->invB)
      continue;
    if (!PathsEqual(*site->invFile, ownerFile))
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

std::optional<RefoldEngine::LineStateObserverSite>
RefoldEngine::FirstOwnerSuffixLineStateObserverSite(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  std::optional<LineStateObserverSite> best;

  auto considerSite = [&](const RefoldModel::MacroInvocation &builtin,
                          const RefoldModel::MacroInvocation &site) {
    const bool observesLine = builtin.name == "__LINE__";
    const bool observesFile = builtin.name == "__FILE__" ||
                              builtin.name == "__BASE_FILE__";
    const bool observesFileName = builtin.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      return;
    if (site.ownerIncludeId != ownerIncludeId)
      return;
    if (!site.invFile || !site.invB)
      return;
    if (!PathsEqual(*site.invFile, ownerFile))
      return;
    if (*site.invB < offset)
      return;
    if (IsInvocationInsideDefineDirective(site))
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
    const bool observesFile = macro.name == "__FILE__" ||
                              macro.name == "__BASE_FILE__";
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
      site = FindMacroInvocationById(*site->callerMacroId);
    }
  }

  return best;
}

bool RefoldEngine::OwnerSuffixHasLineStateSensitiveBuiltin(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  return OwnerSuffixLineStateObserverDemand(ownerIncludeId, ownerFile, offset)
      .Any();
}


std::optional<uint64_t>
RefoldEngine::AdvanceInsertionAnchorPastSourceLineControlPrefix(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    StringRef ownerBytes, uint64_t anchor) const {
  if (anchor > ownerBytes.size())
    return std::nullopt;

  uint64_t cur = anchor;
  bool advanced = false;

  while (true) {
    const RefoldModel::LineControlEvent *best = nullptr;

    for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
      if (!event.active || !event.producerProven || !event.siteB ||
          !event.siteE)
        continue;
      if (event.ownerIncludeId != ownerIncludeId)
        continue;
      if (!PathsEqual(event.physicalFile, ownerFile))
        continue;
      if (*event.siteB < cur || *event.siteE <= cur ||
          *event.siteE > ownerBytes.size())
        continue;

      // Only slide across zero-token whitespace before the directive.  Comments
      // or other trivia may be intentionally positioned before the directive and
      // must not be silently crossed by this source-placement rule.
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


bool RefoldEngine::MaybeAdvanceTUInsertionPastSourceLineControlPrefix(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    std::pair<uint64_t, uint64_t> &span, StringRef tracePrefix) const {
  if (!h.isInsertOnly() || span.first != span.second ||
      IsPPGapAtSelectedConditionalArmExit(h.aStart))
    return false;

  std::optional<uint64_t> exactAnchor =
      AnchorToExactSlotBoundaryFromPPGap(tuPath, h.aStart);
  if (!exactAnchor || *exactAnchor != span.first)
    return false;

  std::optional<uint64_t> advancedAnchor =
      AdvanceInsertionAnchorPastSourceLineControlPrefix(tuPath, std::nullopt,
                                                        tuBytes, span.first);
  if (!advancedAnchor)
    return false;

  span.first = *advancedAnchor;
  span.second = *advancedAnchor;
  return true;
}

bool RefoldEngine::TUInsertionCanDeferResyncToConditionalJoin(
    bool advancedOverSourceLineControlPrefix, StringRef tuPath, uint64_t anchor,
    StringRef tracePrefix) const {
  if (!advancedOverSourceLineControlPrefix)
    return false;

  std::optional<LineStateObserverSite> firstObserver =
      FirstOwnerSuffixLineStateObserverSite(std::nullopt, tuPath, anchor);
  if (!firstObserver || !firstObserver->demand.needsLine)
    return false;

  const RefoldModel::CondGroup *innermost = nullptr;
  for (const RefoldModel::CondGroup *group :
       model_.GetCondGroups(tuPath, std::nullopt)) {
    if (!group || !PathsEqual(group->file, tuPath) || group->parentIncludeId ||
        !group->ContainsByte(anchor)) {
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

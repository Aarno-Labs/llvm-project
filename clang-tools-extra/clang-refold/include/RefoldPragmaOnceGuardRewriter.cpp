//===--- RefoldPragmaOnceGuardRewriter.cpp ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "include/RefoldPragmaOnceGuardRewriter.h"

#include "proof/RefoldPragmaTaxonomy.h"

#include "core/RefoldLog.h"
#include "edit/RefoldTextEditAssembler.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/SourceLineDirectiveHelpers.h"
#include "proof/RefoldAcceptedCandidateBuilder.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldTerminalProofSink.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Reserved spelling prefix for every synthetic once guard.
///
/// Collision-freedom is proven by showing this exact substring occurs nowhere in
/// the inputs, so the prefix must stay a single literal shared by the scan and
/// the name builder.
constexpr StringRef kGuardMacroPrefix = "__CLANG_REFOLD_ONCE_";

/// Preprocessing kinds a pragma-once guard rewrite may consume at a pragma site.
///
/// `PragmaOperator` is admitted because `_Pragma("once")` establishes exactly
/// the once-state a `#pragma once` directive does.  It differs only in being an
/// expression rather than a directive, which bears on whether its bytes may be
/// rewritten in place, not on what it establishes; `OnceSiteInPlaceReplacement`
/// below decides that separately and declines the site when it cannot.
ArrayRef<PreprocessingStructureKind> pragmaGuardKinds() {
  static constexpr PreprocessingStructureKind kinds[] = {
      PreprocessingStructureKind::Pragma,
      PreprocessingStructureKind::PragmaOperator};
  return kinds;
}

/// Return whether another indexed interval strictly contains \p interval.
///
/// Containment is what makes a site's bytes unavailable: the containing
/// construct owns them as part of its own exact transition, so replacing a
/// sub-range of it would break that construct rather than the site.  A
/// directive can never be contained, so in practice this only ever answers for
/// a `_Pragma` operator.
bool IntervalIsEnclosedByAnotherStructure(
    const RefoldPreprocessingStructureIndex &index,
    const PreprocessingStructureInterval &interval) {
  for (const PreprocessingStructureInterval &other : index.GetIntervals()) {
    if (&other == &interval)
      continue;
    if (other.begin <= interval.begin && interval.end <= other.end &&
        (other.begin < interval.begin || interval.end < other.end))
      return true;
  }
  return false;
}

/// Return the text that replaces one once site's spelling with \p directive,
/// or nothing when the site's bytes may not carry a directive.
///
/// A `#pragma once` directive already owns its logical line, so the directive
/// text alone is the replacement and the site's own terminating newline still
/// ends it.  A `_Pragma("once")` operator is an expression and may share its
/// line, so the replacement opens a line for the directive and, when source
/// follows on the same line, closes one after it.  Each opened line is one
/// physical line of drift in the emitted body, which is admissible here only
/// under the same suffix line-observer proof the `#ifndef` prologue needs.
///
/// Nothing is returned for an operator the index reports as contained by
/// another protected construct: those bytes belong to that construct's exact
/// transition and rewriting them would break it.
std::optional<std::string>
OnceSiteInPlaceReplacement(const PragmaOnceSite &site, StringRef bytes,
                           StringRef directive) {
  if (!site.viaPragmaOperator)
    return directive.str();
  if (site.enclosedByProtectedStructure)
    return std::nullopt;
  if (site.spellingBegin > bytes.size() || site.spellingEnd > bytes.size() ||
      site.spellingBegin > site.spellingEnd)
    return std::nullopt;

  // Deleting an operator never needs a line of its own: removing an expression
  // leaves the rest of the line exactly as it was.
  if (directive.empty())
    return std::string();

  // Bytes of the site's own physical line on each side of the spelling.  The
  // leading side is computed from the last newline explicitly rather than with
  // `rsplit`, which reports the whole prefix as its first element when the
  // string holds no separator at all: a site on the header's first line has no
  // preceding newline, and reading that as "nothing precedes it" would claim
  // the site owns a line it in fact shares.
  const StringRef beforeSpelling = bytes.substr(0, site.spellingBegin);
  const size_t lineBegin = beforeSpelling.rfind('\n');
  const StringRef beforeOnLine = lineBegin == StringRef::npos
                                     ? beforeSpelling
                                     : beforeSpelling.substr(lineBegin + 1);
  const StringRef afterOnLine =
      bytes.substr(site.spellingEnd).split('\n').first;

  std::string replacement;
  if (!beforeOnLine.trim().empty())
    replacement += "\n";
  replacement += directive;
  if (!afterOnLine.trim().empty())
    replacement += "\n";
  return replacement;
}

/// Preprocessing kinds a pragma-once guard rewrite may consume at an include
/// site.  `Import` is intentionally absent: it establishes once-state with no
/// pragma, which this catalog does not model.
ArrayRef<PreprocessingStructureKind> includeGuardKinds() {
  static constexpr PreprocessingStructureKind kinds[] = {
      PreprocessingStructureKind::Include,
      PreprocessingStructureKind::IncludeNext};
  return kinds;
}

/// Return whether \p text names the `once` pragma operand.
///
/// Shared by the directive recognizer and the `_Pragma` operator detector so a
/// single grammar decides what counts as once-state.  That grammar now lives in
/// the pragma taxonomy, which owns every spelling classification, so this stays
/// as a local name for the same rule rather than a second copy of it.
bool operandIsOnceKeyword(StringRef text) {
  return pragmaOperandNamesOnce(text);
}

/// Return whether a `_Pragma` / `__pragma` operator spelling carries `once`.
///
/// This is a detector, not a realizer.  The producer records nothing for
/// `_Pragma("once")`, so recognizing it exists only so the affected header can
/// fail closed instead of silently losing its once-state.
bool pragmaOperatorSpellingIsOnce(StringRef spelling) {
  size_t pos = 0;
  stringutils::skipNonNewlineWs(spelling, pos);

  StringRef rest = spelling.substr(pos);
  if (rest.starts_with("_Pragma"))
    pos += 7;
  else if (rest.starts_with("__pragma"))
    pos += 8;
  else
    return false;

  stringutils::skipNonNewlineWs(spelling, pos);
  if (pos >= spelling.size() || spelling[pos] != '(')
    return false;
  ++pos;

  stringutils::skipNonNewlineWs(spelling, pos);

  // Accept only a simple, directly spelled string literal operand.  A wide or
  // raw literal, a macro-computed operand, or concatenated pieces are all
  // out of domain and must not be silently classified as "not once".
  if (pos >= spelling.size() || spelling[pos] != '"')
    return pos < spelling.size();
  ++pos;

  const size_t contentBegin = pos;
  while (pos < spelling.size() && spelling[pos] != '"') {
    if (spelling[pos] == '\\')
      ++pos;
    ++pos;
  }
  if (pos >= spelling.size())
    return false;

  return operandIsOnceKeyword(spelling.substr(contentBegin, pos - contentBegin));
}

/// Return the guard macro name for one deterministic index.
std::string guardMacroNameForIndex(size_t oneBasedIndex) {
  return (kGuardMacroPrefix + Twine(oneBasedIndex)).str();
}

} // namespace

bool refoldTextIsPragmaOnceDirective(StringRef text) {
  StringRef trimmed = text.trim();
  size_t pos = 0;

  stringutils::skipNonNewlineWs(trimmed, pos);
  if (pos >= trimmed.size() || trimmed[pos] != '#')
    return false;
  ++pos;

  stringutils::skipNonNewlineWs(trimmed, pos);
  if (!trimmed.substr(pos).starts_with("pragma"))
    return false;
  pos += 6;
  if (pos < trimmed.size() && stringutils::isIdentPart(trimmed[pos]))
    return false;

  return operandIsOnceKeyword(trimmed.drop_front(pos));
}

StringRef toString(PragmaOnceGuardRejection rejection) {
  switch (rejection) {
  case PragmaOnceGuardRejection::None:
    return "None";
  case PragmaOnceGuardRejection::PragmaOperatorOnce:
    return "PragmaOperatorOnce";
  case PragmaOnceGuardRejection::ImportEdge:
    return "ImportEdge";
  case PragmaOnceGuardRejection::MissingOpenedPath:
    return "MissingOpenedPath";
  case PragmaOnceGuardRejection::IncompleteStructureCensus:
    return "IncompleteStructureCensus";
  case PragmaOnceGuardRejection::UnboundPragmaRecord:
    return "UnboundPragmaRecord";
  case PragmaOnceGuardRejection::NonDominatingEstablishingSite:
    return "NonDominatingEstablishingSite";
  case PragmaOnceGuardRejection::SidecarPreservedOccurrence:
    return "SidecarPreservedOccurrence";
  case PragmaOnceGuardRejection::SelfIncludingHeader:
    return "SelfIncludingHeader";
  case PragmaOnceGuardRejection::GuardNamespaceCollision:
    return "GuardNamespaceCollision";
  case PragmaOnceGuardRejection::UnaccountedIncludeDirective:
    return "UnaccountedIncludeDirective";
  case PragmaOnceGuardRejection::UnrepairableLineDrift:
    return "UnrepairableLineDrift";
  case PragmaOnceGuardRejection::UnauthorizableIncludeSite:
    return "UnauthorizableIncludeSite";
  }
  return "Unknown";
}

StringRef toString(PragmaOnceTreatment treatment) {
  switch (treatment) {
  case PragmaOnceTreatment::DeletePragma:
    return "DeletePragma";
  case PragmaOnceTreatment::EmitGuard:
    return "EmitGuard";
  }
  return "Unknown";
}

RefoldPragmaOnceGuardRewriter::RefoldPragmaOnceGuardRewriter(
    Dependencies deps, GuardNameInputs inputs)
    : deps_(deps), inputs_(inputs) {
  BuildCandidateCatalog();
  ProveGuardNamespaceIsFree();
}

std::optional<std::string>
RefoldPragmaOnceGuardRewriter::CanonicalPhysicalPathForInclude(
    const RefoldModel::IncludeItem &include) const {
  // Physical identity must come from opened_path.  Legacy resolved_path is a
  // path *spelling*, so two spellings of one header would mint two guards and
  // duplicate the body; that case fails closed instead.
  if (!include.openedPath || include.openedPath->empty())
    return std::nullopt;
  return deps_.pathIdentity.GetCanonicalPath(*include.openedPath).str();
}

std::optional<StringRef>
RefoldPragmaOnceGuardRewriter::LoadHeaderBytes(StringRef physicalPath,
                                               StringRef loadPath) const {
  auto it = headerBytesCache_.find(physicalPath);
  if (it != headerBytesCache_.end()) {
    if (!it->second)
      return std::nullopt;
    return StringRef(*it->second);
  }

  auto bufOrErr =
      MemoryBuffer::getFile(deps_.lineDirs.ToAbsolutePath(loadPath));
  if (!bufOrErr) {
    REFOLD_LOG_TRACE("pragma/once/guard",
                     "cannot read header '{0}' (load path '{1}'): {2}",
                     physicalPath, loadPath, bufOrErr.getError().message());
    headerBytesCache_[physicalPath] = std::nullopt;
    return std::nullopt;
  }

  const MemoryBuffer &mb = **bufOrErr;
  headerBytesCache_[physicalPath] =
      std::string(mb.getBufferStart(), mb.getBufferEnd());
  return StringRef(*headerBytesCache_[physicalPath]);
}

std::optional<uint64_t> RefoldPragmaOnceGuardRewriter::InnermostEnclosingArm(
    StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
    uint64_t byteOffset) const {
  // Conditional groups are instance-scoped through parentIncludeId, so the
  // owner occurrence must match exactly.  Two instances of one header carry
  // separate groups over the same byte ranges, and answering from the wrong
  // instance would attribute a site to an unrelated arm.
  std::optional<uint64_t> innermost;
  uint64_t innermostBodySize = 0;

  for (const RefoldModel::CondGroup &group : deps_.model.GetConds()) {
    if (!deps_.pathIdentity.PathsEqual(group.file, sourcePath))
      continue;
    if (group.parentIncludeId != ownerIncludeId)
      continue;

    for (const RefoldModel::CondArm &arm : group.arms) {
      if (!arm.ContainsByte(byteOffset))
        continue;
      // Nested groups produce nested arms over the same offset; the smallest
      // containing body is the innermost one.  Arm bodies of one group are
      // disjoint, so this comparison never has to break a tie between siblings.
      const uint64_t bodySize = arm.bodyE - arm.bodyB;
      if (!innermost || bodySize < innermostBodySize) {
        innermost = arm.id;
        innermostBodySize = bodySize;
      }
    }
  }

  return innermost;
}

bool RefoldPragmaOnceGuardRewriter::DiscoverPragmaOnceSites(
    StringRef physicalPath, StringRef sourcePath,
    std::optional<uint64_t> ownerIncludeId, StringRef bytes,
    SmallVectorImpl<PragmaOnceSite> &sites,
    PragmaOnceGuardRejection &rejection, std::string &detail) const {
  sites.clear();
  rejection = PragmaOnceGuardRejection::None;
  detail.clear();

  const RefoldPreprocessingStructureIndex index =
      RefoldPreprocessingStructureIndex::Build(
          RefoldPreprocessingStructureIndex::Dependencies{
              deps_.model, deps_.pathIdentity, deps_.macroStateProof,
              deps_.lexLang},
          sourcePath, bytes, ownerIncludeId);

  // An incomplete protection census means the pragma inventory itself cannot be
  // trusted to be complete, and a missed `#pragma once` is exactly the case that
  // duplicates a header body.
  if (!index.IsProtectionCensusComplete()) {
    rejection = PragmaOnceGuardRejection::IncompleteStructureCensus;
    detail = formatv("header '{0}' has an incomplete protection census: {1}",
                     physicalPath,
                     index.GetDirectTUProtectionDiagnostics().empty()
                         ? StringRef("unknown")
                         : StringRef(index.GetDirectTUProtectionDiagnostics()[0]))
                 .str();
    return false;
  }

  for (const PreprocessingStructureInterval &interval : index.GetIntervals()) {
    const StringRef spelling = bytes.substr(
        interval.structureSpellingBegin,
        interval.structureSpellingEnd - interval.structureSpellingBegin);

    // `_Pragma("once")` establishes the same once-state as `#pragma once`, so
    // it is admitted on the same terms -- but only against an exact producer
    // binding.  The binding is what proves this operator actually executed in
    // this include-owner domain; the spelling alone does not, because the same
    // bytes could sit in a macro replacement list or in a conditional arm that
    // was never taken.  Unbound, the header's once-state cannot be accounted
    // for, and a missed once site is exactly the case that duplicates a body.
    if (interval.kind == PreprocessingStructureKind::PragmaOperator) {
      if (!pragmaOperatorSpellingIsOnce(spelling))
        continue;

      if (!interval.IsProducerBound()) {
        rejection = PragmaOnceGuardRejection::PragmaOperatorOnce;
        detail = formatv("header '{0}' establishes once-state through a pragma "
                         "operator at [{1},{2}) with no exact producer binding",
                         physicalPath, interval.structureSpellingBegin,
                         interval.structureSpellingEnd)
                     .str();
        return false;
      }

      PragmaOnceSite site;
      site.begin = interval.begin;
      site.end = interval.end;
      site.spellingBegin = interval.structureSpellingBegin;
      site.spellingEnd = interval.structureSpellingEnd;
      site.modelItemId = interval.modelItemId;
      site.enclosingArmId =
          InnermostEnclosingArm(sourcePath, ownerIncludeId, interval.begin);
      site.viaPragmaOperator = true;
      site.enclosedByProtectedStructure =
          IntervalIsEnclosedByAnotherStructure(index, interval);
      sites.push_back(site);
      continue;
    }

    if (interval.kind != PreprocessingStructureKind::Pragma)
      continue;
    if (!refoldTextIsPragmaOnceDirective(spelling))
      continue;

    PragmaOnceSite site;
    site.begin = interval.begin;
    site.end = interval.end;
    site.spellingBegin = interval.structureSpellingBegin;
    site.spellingEnd = interval.structureSpellingEnd;
    if (interval.modelKind == PreprocessingStructureModelKind::PragmaDirective)
      site.modelItemId = interval.modelItemId;
    site.enclosingArmId =
        InnermostEnclosingArm(sourcePath, ownerIncludeId, interval.begin);
    sites.push_back(site);
  }

  // Every producer `#pragma once` record for this physical path must be
  // accounted for by a discovered interval.  A record with no matching interval
  // means the lexical inventory and the producer disagree, so the header's
  // once-state is not fully understood.
  for (const RefoldModel::PragmaDirective &pragma : deps_.model.GetPragmas()) {
    if (!deps_.pathIdentity.PathsEqual(pragma.sitePath, sourcePath))
      continue;
    if (!refoldTextIsPragmaOnceDirective(pragma.text))
      continue;
    // An operator-spelled record anchors at its own exact operator range, not
    // at `siteB`: `siteB` is the start of the whole physical line, which for a
    // mid-line operator lies outside every discovered site.  A record with no
    // recoverable operator range binds to no interval and so stays uncovered,
    // which is the intended fail-closed answer.
    const uint64_t recordAnchor = pragma.viaPragmaOperator && pragma.operatorB
                                      ? *pragma.operatorB
                                      : pragma.siteB;

    const bool covered = llvm::any_of(sites, [&](const PragmaOnceSite &site) {
      return site.begin <= recordAnchor && recordAnchor < site.end;
    });
    if (!covered) {
      rejection = PragmaOnceGuardRejection::UnboundPragmaRecord;
      detail =
          formatv("header '{0}' producer pragma id={1} site=[{2},{3}) matched "
                  "no discovered once interval",
                  physicalPath, pragma.id, pragma.siteB, pragma.siteE)
              .str();
      return false;
    }
  }

  return true;
}

bool RefoldPragmaOnceGuardRewriter::HeaderIncludesItself(
    StringRef physicalPath) const {
  if (auto it = selfIncludeCache_.find(physicalPath);
      it != selfIncludeCache_.end()) {
    return it->second;
  }
  // Seed the memo with `false` before recursing so a cyclic include graph
  // terminates instead of re-entering this query for the same header.
  selfIncludeCache_[physicalPath] = false;

  // A self-include is the only construct that can observe once-state between a
  // header's first byte and a bottom-of-file `#pragma once`, which is what makes
  // a top-of-body `#define` inexact for a body realized from B.
  DenseSet<uint64_t> visited;
  SmallVector<uint64_t, 8> worklist;

  auto pathOf = [&](const RefoldModel::IncludeItem &include) {
    return CanonicalPhysicalPathForInclude(include);
  };

  for (const RefoldModel::IncludeItem &include : deps_.model.GetIncludes()) {
    if (!deps_.pathIdentity.PathsEqual(include.sitePath, physicalPath))
      continue;
    worklist.push_back(include.id);
  }

  while (!worklist.empty()) {
    const uint64_t id = worklist.pop_back_val();
    if (!visited.insert(id).second)
      continue;

    const RefoldModel::IncludeItem *edge = deps_.model.GetIncludeById(id);
    if (!edge)
      continue;

    std::optional<std::string> opened = pathOf(*edge);
    if (!opened)
      continue;
    if (deps_.pathIdentity.PathsEqual(*opened, physicalPath)) {
      selfIncludeCache_[physicalPath] = true;
      return true;
    }

    for (const RefoldModel::IncludeItem &nested : deps_.model.GetIncludes())
      if (deps_.pathIdentity.PathsEqual(nested.sitePath, *opened))
        worklist.push_back(nested.id);
  }

  return false;
}

bool RefoldPragmaOnceGuardRewriter::ProveNoUnaccountedIncludeDirectives(
    std::string &detail) const {
  if (unaccountedIncludeProof_) {
    detail = unaccountedIncludeProof_->second;
    return unaccountedIncludeProof_->first;
  }

  // Collect every physical source this run touches.  An unaccounted include in
  // any of them could name any header, so the proof is whole-run.
  //
  // Each header must be indexed in the owner domain of the instance the producer
  // actually entered.  Producer bindings are owner-scoped, and a header's own
  // conditional groups belong to a concrete instance, so scanning a header with
  // no owner would fail the census for a purely bookkeeping reason.
  struct SourceToScan {
    std::string loadPath;
    std::optional<uint64_t> ownerIncludeId;
  };
  std::map<std::string, SourceToScan> sources;
  sources[deps_.pathIdentity.GetCanonicalPath(inputs_.tuPath).str()] =
      SourceToScan{inputs_.tuPath.str(), std::nullopt};
  for (const RefoldModel::IncludeItem &include : deps_.model.GetIncludes()) {
    std::optional<std::string> canonical =
        CanonicalPhysicalPathForInclude(include);
    if (!canonical)
      continue;
    SourceToScan &entry = sources[*canonical];
    if (entry.loadPath.empty()) {
      entry.loadPath =
          include.openedPath ? include.openedPath->str() : *canonical;
    }
    // `enteredFileName` is set only from the file-enter callback, so it marks the
    // one instance whose producer records exist for this header's contents.
    if (include.enteredFileName && !entry.ownerIncludeId)
      entry.ownerIncludeId = include.id;
  }

  // Owner-agnostic record lookup.  The question is whether the preprocessor ever
  // recorded a directive at this lexical site, not which instance owns it: a
  // skipped nested edge legitimately has no parent and therefore binds in no
  // owner domain at all, yet it *is* recorded and does count as an occurrence.
  auto lexicalIncludeIsRecorded = [&](StringRef sourcePath, uint64_t begin,
                                      uint64_t end) {
    return llvm::any_of(
        deps_.model.GetIncludes(),
        [&](const RefoldModel::IncludeItem &include) {
          return deps_.pathIdentity.PathsEqual(include.sitePath, sourcePath) &&
                 begin <= include.siteB && include.siteB < end;
        });
  };

  auto record = [&](bool proven, std::string why) {
    unaccountedIncludeProof_ = std::make_pair(proven, why);
    detail = std::move(why);
    return proven;
  };

  for (const auto &entry : sources) {
    std::optional<StringRef> bytes =
        LoadHeaderBytes(entry.first, entry.second.loadPath);
    if (!bytes) {
      return record(false, formatv("cannot read source '{0}' to prove its "
                                   "include directives are accounted for",
                                   entry.first)
                               .str());
    }

    const RefoldPreprocessingStructureIndex index =
        RefoldPreprocessingStructureIndex::Build(
            RefoldPreprocessingStructureIndex::Dependencies{
                deps_.model, deps_.pathIdentity, deps_.macroStateProof,
                deps_.lexLang},
            entry.first, *bytes, entry.second.ownerIncludeId);

    if (!index.IsProtectionCensusComplete()) {
      return record(false,
                    formatv("source '{0}' has an incomplete protection census, "
                            "so its include directives cannot be enumerated",
                            entry.first)
                        .str());
    }

    for (const PreprocessingStructureInterval &interval : index.GetIntervals()) {
      const bool isIncludeDirective =
          interval.kind == PreprocessingStructureKind::Include ||
          interval.kind == PreprocessingStructureKind::IncludeNext ||
          interval.kind == PreprocessingStructureKind::Import;
      if (!isIncludeDirective)
        continue;

      // A recorded directive is one the preprocessor actually reached, so its
      // target is already counted among this header's occurrences.  A directive
      // with no record at all is invisible to occurrence counting: that is the
      // unreached-conditional-arm case, whose include is never lexed.
      if (lexicalIncludeIsRecorded(entry.first, interval.begin, interval.end))
        continue;

      return record(
          false,
          formatv("source '{0}' has an unaccounted {1} directive at [{2},{3})",
                  entry.first, toString(interval.kind),
                  interval.structureSpellingBegin,
                  interval.structureSpellingEnd)
              .str());
    }
  }

  return record(true, std::string());
}

bool RefoldPragmaOnceGuardRewriter::EstablishingSiteIsUnconditional(
    ArrayRef<PragmaOnceSite> sites,
    std::optional<uint64_t> ancestorArmId) const {
  // A site inside the header is only unconditional if the header itself was also
  // reached unconditionally; otherwise the whole body sits in an arm that some
  // other configuration may not take.
  if (ancestorArmId)
    return false;
  return llvm::any_of(sites, [](const PragmaOnceSite &site) {
    return !site.enclosingArmId.has_value();
  });
}

bool RefoldPragmaOnceGuardRewriter::GuardLineDriftIsRepairable(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerPath,
    uint64_t offset) const {
  // With `#line` injection available the ordinary resync machinery repairs the
  // added physical lines.
  if (deps_.lineDirs.Enabled())
    return true;
  // Otherwise the drift is permanent, so it is admissible only when nothing in
  // the shifted suffix observes line state.
  return !deps_.lineControlProof.OwnerSuffixHasLineStateSensitiveBuiltin(
      ownerIncludeId, ownerPath, offset);
}

void RefoldPragmaOnceGuardRewriter::RejectCandidate(
    HeaderCandidate &candidate, PragmaOnceGuardRejection reason,
    std::string detail) {
  if (candidate.guard.rejection != PragmaOnceGuardRejection::None)
    return;
  candidate.guard.rejection = reason;
  candidate.guard.rejectionDetail = std::move(detail);
  REFOLD_LOG_TRACE("pragma/once/guard", "reject header '{0}': {1} ({2})",
                   candidate.guard.physicalHeaderPath, toString(reason),
                   candidate.guard.rejectionDetail);
}

void RefoldPragmaOnceGuardRewriter::BuildCandidateCatalog() {
  // Group include edges by physical header identity first.  An edge that cannot
  // be keyed physically is recorded against the header it names so the
  // fail-closed decision survives, because guarding some occurrences of a header
  // but not others is exactly the unsound outcome.
  for (const RefoldModel::IncludeItem &include : deps_.model.GetIncludes()) {
    std::optional<std::string> canonical =
        CanonicalPhysicalPathForInclude(include);
    if (!canonical) {
      // Without opened_path there is no physical key at all.  Fall back to the
      // legacy spelling purely to name a rejection bucket; it is never used to
      // mint a guard.
      const StringRef spelling =
          include.resolvedPath ? *include.resolvedPath : include.target;
      if (spelling.empty())
        continue;
      HeaderCandidate &candidate = candidates_[spelling.str()];
      candidate.guard.physicalHeaderPath = spelling.str();
      candidate.includeIds.push_back(include.id);
      RejectCandidate(candidate, PragmaOnceGuardRejection::MissingOpenedPath,
                      formatv("include edge id={0} target='{1}' has no "
                              "opened_path for physical identity",
                              include.id, include.target)
                          .str());
      continue;
    }

    // A main-file `#pragma once` establishes nothing and Clang warns on it, so
    // the TU is never a guard candidate.
    if (deps_.pathIdentity.PathsEqual(*canonical, inputs_.tuPath))
      continue;

    HeaderCandidate &candidate = candidates_[*canonical];
    candidate.guard.physicalHeaderPath = *canonical;
    candidate.includeIds.push_back(include.id);

    // `#import` carries once-semantics with no pragma at all, so a header
    // reached that way cannot be accounted for by a pragma inventory.
    if (include.subkind == "#import") {
      RejectCandidate(candidate, PragmaOnceGuardRejection::ImportEdge,
                      formatv("include edge id={0} is an #import of '{1}'",
                              include.id, *canonical)
                          .str());
    }
  }

  // Now discover the once inventory for each candidate from its own bytes.
  for (auto &entry : candidates_) {
    HeaderCandidate &candidate = entry.second;
    if (candidate.includeIds.empty())
      continue;

    const RefoldModel::IncludeItem *representative =
        deps_.model.GetIncludeById(candidate.includeIds.front());
    if (!representative)
      continue;

    // Prefer the instance that the producer actually entered, because that is
    // the one whose pragma records exist.  Producer pragma records always bind
    // to the entering instance; a skipped instance has none.
    const std::optional<uint64_t> ownerIncludeId =
        ProducerEnteredIncludeIdForPath(entry.first);

    const std::string loadPath =
        representative->openedPath ? representative->openedPath->str()
                                   : entry.first;
    std::optional<StringRef> bytes =
        LoadHeaderBytes(entry.first, loadPath);
    if (!bytes) {
      // A header whose bytes cannot be read has no provable inventory.  Only
      // record that as a rejection if some edge could otherwise have qualified.
      RejectCandidate(candidate,
                      PragmaOnceGuardRejection::IncompleteStructureCensus,
                      formatv("cannot read header '{0}'", entry.first).str());
      continue;
    }

    const StringRef sourcePath = representative->enteredFileSpelling
                                     ? *representative->enteredFileSpelling
                                     : StringRef(entry.first);

    SmallVector<PragmaOnceSite, 2> sites;
    PragmaOnceGuardRejection rejection = PragmaOnceGuardRejection::None;
    std::string detail;
    if (!DiscoverPragmaOnceSites(entry.first, sourcePath, ownerIncludeId,
                                 *bytes, sites, rejection, detail)) {
      RejectCandidate(candidate, rejection, std::move(detail));
      continue;
    }

    candidate.guard.sites = std::move(sites);
    candidate.guard.unconditionalPragmaProven =
        llvm::any_of(candidate.guard.sites, [](const PragmaOnceSite &site) {
          return !site.enclosingArmId.has_value();
        });
    candidate.guard.recordedOccurrences = candidate.includeIds.size();

    // A header with exactly one provable occurrence needs no guard: nothing can
    // re-enter it and nothing can duplicate it.  Claiming that requires the
    // occurrence count to be complete, which fails when any include directive in
    // the run was never reached by the preprocessor and therefore never recorded.
    if (candidate.guard.recordedOccurrences <= 1) {
      std::string unaccountedDetail;
      if (ProveNoUnaccountedIncludeDirectives(unaccountedDetail)) {
        candidate.guard.treatment = PragmaOnceTreatment::DeletePragma;
      } else {
        candidate.guard.treatment = PragmaOnceTreatment::EmitGuard;
        REFOLD_LOG_TRACE("pragma/once/guard",
                         "header '{0}' has one recorded occurrence but keeps a "
                         "guard: {1}",
                         entry.first, unaccountedDetail);
      }
    } else {
      candidate.guard.treatment = PragmaOnceTreatment::EmitGuard;
    }
  }

  // Drop candidates that establish no once-state and carry no rejection: they
  // are ordinary headers and must not appear in the catalog at all.
  for (auto it = candidates_.begin(); it != candidates_.end();) {
    const HeaderCandidate &candidate = it->second;
    if (candidate.guard.sites.empty() &&
        candidate.guard.rejection == PragmaOnceGuardRejection::None) {
      it = candidates_.erase(it);
      continue;
    }
    ++it;
  }
}

void RefoldPragmaOnceGuardRewriter::ProveGuardNamespaceIsFree() {
  // Collision-freedom proof: if the reserved prefix occurs nowhere in the TU
  // source, either preprocessed stream, any candidate header's bytes, or any
  // producer macro name, then no name built from that prefix can collide with
  // anything the refolded TU can observe.
  auto prefixOccursIn = [](StringRef text) {
    return text.contains(kGuardMacroPrefix);
  };

  std::string collisionDetail;
  if (prefixOccursIn(inputs_.tuBytes))
    collisionDetail = "translation-unit source";
  else if (prefixOccursIn(inputs_.aSource))
    collisionDetail = "preprocessed stream A";
  else if (prefixOccursIn(inputs_.bSource))
    collisionDetail = "preprocessed stream B";

  if (collisionDetail.empty()) {
    for (const RefoldModel::MacroDirective &directive :
         deps_.model.GetMacroDirectives()) {
      if (directive.name.starts_with(kGuardMacroPrefix)) {
        collisionDetail =
            formatv("producer macro directive id={0} name='{1}'", directive.id,
                    directive.name)
                .str();
        break;
      }
    }
  }

  if (collisionDetail.empty()) {
    for (const auto &entry : candidates_) {
      std::optional<StringRef> bytes = LoadHeaderBytes(entry.first, entry.first);
      if (bytes && prefixOccursIn(*bytes)) {
        collisionDetail =
            formatv("header '{0}' source bytes", entry.first).str();
        break;
      }
    }
  }

  if (!collisionDetail.empty()) {
    for (auto &entry : candidates_)
      RejectCandidate(entry.second,
                      PragmaOnceGuardRejection::GuardNamespaceCollision,
                      formatv("reserved guard prefix '{0}' already occurs in {1}",
                              kGuardMacroPrefix, collisionDetail)
                          .str());
  }
}

void RefoldPragmaOnceGuardRewriter::AssignGuardNames() {
  // Number only the headers that are actually inlined and actually emit a guard.
  // `candidates_` is a std::map over canonical paths, so the traversal order does
  // not depend on include discovery order or on any hash seed.
  size_t nextIndex = 1;
  for (auto &entry : candidates_) {
    HeaderCandidate &candidate = entry.second;
    if (!candidate.active ||
        candidate.guard.rejection != PragmaOnceGuardRejection::None ||
        candidate.guard.sites.empty()) {
      continue;
    }
    // A `DeletePragma` header consumes no guard number, so numbering stays
    // dense over the headers that actually emit a macro.  This keeps expected
    // outputs stable when an unrelated single-occurrence header is added.
    if (candidate.guard.treatment == PragmaOnceTreatment::DeletePragma) {
      REFOLD_LOG_TRACE("pragma/once/guard",
                       "header '{0}' treatment={1} sites={2} occurrences={3}",
                       entry.first, toString(candidate.guard.treatment),
                       candidate.guard.sites.size(),
                       candidate.guard.recordedOccurrences);
      continue;
    }
    candidate.guard.macroName = guardMacroNameForIndex(nextIndex++);
    REFOLD_LOG_TRACE("pragma/once/guard",
                     "guard '{0}' for header '{1}' sites={2} occurrences={3} "
                     "unconditional={4}",
                     candidate.guard.macroName, entry.first,
                     candidate.guard.sites.size(),
                     candidate.guard.recordedOccurrences,
                     candidate.guard.unconditionalPragmaProven ? 1 : 0);
  }
}

void RefoldPragmaOnceGuardRewriter::SetActiveGuardedHeaders(
    ArrayRef<StringRef> physicalPaths) {
  if (activeSetRecorded_) {
    REFOLD_LOG_FATAL("pragma/once/guard",
                     "active guarded-header set recorded more than once");
  }
  activeSetRecorded_ = true;

  for (StringRef path : physicalPaths) {
    const std::string canonical =
        deps_.pathIdentity.GetCanonicalPath(path).str();
    if (HeaderCandidate *candidate = FindCandidate(canonical))
      candidate->active = true;
  }

  // Guard names are assigned only now, so numbering covers exactly the headers
  // that emit a guard in this run.  A once-header that nothing inlined never
  // engages a guard at all and never consumes a number.
  AssignGuardNames();
}


RefoldPragmaOnceGuardRewriter::HeaderCandidate *
RefoldPragmaOnceGuardRewriter::FindCandidate(StringRef canonicalPath) {
  auto it = candidates_.find(canonicalPath.str());
  return it == candidates_.end() ? nullptr : &it->second;
}

const RefoldPragmaOnceGuardRewriter::HeaderCandidate *
RefoldPragmaOnceGuardRewriter::FindCandidate(StringRef canonicalPath) const {
  auto it = candidates_.find(canonicalPath.str());
  return it == candidates_.end() ? nullptr : &it->second;
}

const PragmaOnceGuard *
RefoldPragmaOnceGuardRewriter::FindGuardForPath(StringRef physicalPath) const {
  const std::string canonical =
      deps_.pathIdentity.GetCanonicalPath(physicalPath).str();
  const HeaderCandidate *candidate = FindCandidate(canonical);
  if (!candidate || !candidate->active || !candidate->guard.IsUsable())
    return nullptr;
  return &candidate->guard;
}

std::optional<uint64_t>
RefoldPragmaOnceGuardRewriter::ProducerEnteredIncludeIdForPath(
    StringRef canonicalPath) const {
  const HeaderCandidate *candidate = FindCandidate(canonicalPath);
  if (!candidate)
    return std::nullopt;
  for (uint64_t id : candidate->includeIds) {
    const RefoldModel::IncludeItem *edge = deps_.model.GetIncludeById(id);
    if (edge && edge->enteredFileName)
      return id;
  }
  return std::nullopt;
}

const PragmaOnceGuard *RefoldPragmaOnceGuardRewriter::FindGuardForInclude(
    const RefoldModel::IncludeItem &include) const {
  std::optional<std::string> canonical =
      CanonicalPhysicalPathForInclude(include);
  if (!canonical)
    return nullptr;
  return FindGuardForPath(*canonical);
}

bool RefoldPragmaOnceGuardRewriter::HeaderRequiresGuard(
    StringRef physicalPath) const {
  return FindGuardForPath(physicalPath) != nullptr;
}

PragmaOnceGuardRejection
RefoldPragmaOnceGuardRewriter::FindRejectionForInclude(
    const RefoldModel::IncludeItem &include) const {
  std::optional<std::string> canonical =
      CanonicalPhysicalPathForInclude(include);
  if (!canonical) {
    const StringRef spelling =
        include.resolvedPath ? *include.resolvedPath : include.target;
    const HeaderCandidate *candidate = FindCandidate(spelling);
    return candidate ? candidate->guard.rejection
                     : PragmaOnceGuardRejection::MissingOpenedPath;
  }

  const HeaderCandidate *candidate = FindCandidate(*canonical);
  return candidate ? candidate->guard.rejection
                   : PragmaOnceGuardRejection::None;
}

bool RefoldPragmaOnceGuardRewriter::IncludeLiesInsideHeader(
    uint64_t ownerIncludeId, StringRef physicalPath) const {
  // Walking parents upward is exact and index-free: an owner lies inside the
  // header exactly when some ancestor instance -- or the owner itself -- opened
  // that physical path.  Repeated instances of the header are all covered
  // because each is an ancestor of its own subtree.
  DenseSet<uint64_t> visited;
  std::optional<uint64_t> current = ownerIncludeId;
  while (current) {
    if (!visited.insert(*current).second)
      break;
    const RefoldModel::IncludeItem *include =
        deps_.model.GetIncludeById(*current);
    if (!include)
      break;
    if (include->openedPath &&
        deps_.pathIdentity.PathsEqual(*include->openedPath, physicalPath))
      return true;
    current = include->parent;
  }
  return false;
}

bool RefoldPragmaOnceGuardRewriter::HeaderMacroStateIsObservedOutside(
    StringRef physicalPath) const {
  llvm::StringSet<> definedInside;
  for (const RefoldModel::MacroDirective &directive :
       deps_.model.GetMacroDirectives()) {
    if (directive.ownerIncludeId &&
        IncludeLiesInsideHeader(*directive.ownerIncludeId, physicalPath))
      definedInside.insert(directive.name);
  }
  if (definedInside.empty())
    return false;

  // An invocation of one of those names from outside the subtree means the
  // definition is live after the header: suppressing a later re-entry would
  // leave that use undefined.  Invocations inside the header's own instances do
  // not count, because a body realized from B already carries their expansion.
  for (const RefoldModel::MacroInvocation &invocation :
       deps_.model.GetMacroInvocations()) {
    if (!definedInside.contains(invocation.name))
      continue;
    if (invocation.ownerIncludeId &&
        IncludeLiesInsideHeader(*invocation.ownerIncludeId, physicalPath))
      continue;
    REFOLD_LOG_TRACE("pragma/once/guard",
                     "header '{0}' macro '{1}' is observed outside it; its "
                     "include-guard state cannot be restored",
                     physicalPath, invocation.name);
    return true;
  }
  return false;
}

bool RefoldPragmaOnceGuardRewriter::HeaderEstablishesOnceState(
    StringRef physicalPath) const {
  const std::string canonical =
      deps_.pathIdentity.GetCanonicalPath(physicalPath).str();
  const HeaderCandidate *candidate = FindCandidate(canonical);
  return candidate && !candidate->guard.sites.empty() &&
         candidate->guard.rejection == PragmaOnceGuardRejection::None;
}

bool RefoldPragmaOnceGuardRewriter::HeaderEstablishesReentryProtection(
    StringRef physicalPath) const {
  if (HeaderEstablishesOnceState(physicalPath))
    return true;

  // A classic guard is recorded on the include instance rather than on the
  // header, so ask whether any instance opening this file named one.
  for (const RefoldModel::IncludeItem &include : deps_.model.GetIncludes()) {
    if (!include.controllingMacro || include.controllingMacro->empty())
      continue;
    std::optional<std::string> canonical =
        CanonicalPhysicalPathForInclude(include);
    if (canonical &&
        deps_.pathIdentity.PathsEqual(*canonical, physicalPath))
      return true;
  }
  return false;
}

bool RefoldPragmaOnceGuardRewriter::IncludeClosureReentersHeader(
    const RefoldModel::IncludeItem &include, ArrayRef<std::string> targetPaths,
    std::string *reenteredPath) const {
  if (targetPaths.empty())
    return false;

  std::optional<std::string> rootPath =
      CanonicalPhysicalPathForInclude(include);
  if (!rootPath)
    return false;

  auto isTarget = [&](StringRef path) {
    return llvm::any_of(targetPaths, [&](const std::string &target) {
      return deps_.pathIdentity.PathsEqual(path, target);
    });
  };

  // Walk physical paths, not include ids: the same header can be entered by
  // several instances, and a skipped edge has no parent to walk from.
  llvm::StringSet<> visited;
  SmallVector<std::string, 16> worklist;
  worklist.push_back(*rootPath);
  visited.insert(*rootPath);

  while (!worklist.empty()) {
    const std::string current = worklist.pop_back_val();

    for (const RefoldModel::IncludeItem &edge : deps_.model.GetIncludes()) {
      if (!deps_.pathIdentity.PathsEqual(edge.sitePath, current))
        continue;
      std::optional<std::string> opened = CanonicalPhysicalPathForInclude(edge);
      if (!opened)
        continue;

      // The root header being a target is the *direct* case, which the ordinary
      // surviving-include wrapper already handles.  Only a nested re-entry is
      // out of the guard's reach.
      if (isTarget(*opened)) {
        if (reenteredPath)
          *reenteredPath = *opened;
        return true;
      }
      if (visited.insert(*opened).second)
        worklist.push_back(*opened);
    }
  }

  return false;
}

std::vector<std::string>
RefoldPragmaOnceGuardRewriter::ActiveGuardedHeaderPaths() const {
  std::vector<std::string> paths;
  for (const auto &entry : candidates_)
    if (entry.second.active && entry.second.guard.IsUsable() &&
        entry.second.guard.treatment == PragmaOnceTreatment::EmitGuard)
      paths.push_back(entry.first);
  return paths;
}

bool RefoldPragmaOnceGuardRewriter::AppendRealizedFromBIncludeGuardRestoration(
    ArrayRef<uint64_t> enteredSubtreeIncludeIds,
    std::string &realizedBody) const {
  // Collect one restoration per physical header, in canonical-path order, so
  // repeated instances of a header do not emit the define twice and the output
  // does not depend on include traversal order.
  std::map<std::string, std::string> guardByPath;

  for (uint64_t includeId : enteredSubtreeIncludeIds) {
    const RefoldModel::IncludeItem *include =
        deps_.model.GetIncludeById(includeId);
    if (!include)
      continue;
    std::optional<std::string> canonical =
        CanonicalPhysicalPathForInclude(*include);
    if (!canonical)
      continue;

    // A `#pragma once` header has no guard macro to restore; its inlined body is
    // covered by the synthetic guard instead.
    if (HeaderEstablishesOnceState(*canonical))
      continue;

    if (include->controllingMacro && !include->controllingMacro->empty()) {
      // Priming the guard suppresses re-entry into the header and its whole
      // include closure.  A realized-from-B body dropped every `#define` in that
      // closure, so the prime is admissible only when nothing outside still
      // observes those macros; otherwise the later `#include` must stay free to
      // re-enter and re-establish them.  The re-entry hazard that remains is
      // carried by the materialization scheduler's unprotected-header proof,
      // which admits restoration under this same condition.
      if (!HeaderMacroStateIsObservedOutside(*canonical)) {
        guardByPath[*canonical] = include->controllingMacro->str();
        continue;
      }
    }

    // Reaching here means the header is not restorable at this site: either it
    // was never self-protecting -- no controlling macro and no once-state, so
    // the original preprocessing would have re-entered it too and there is
    // nothing to restore -- or its macro state is observed outside it and the
    // prime above was refused.  Neither is failed closed here; the empty marker
    // only reserves the path so a later instance of the same header cannot
    // overwrite a restoration already chosen for it.  The residual re-entry
    // hazard of the second case is proven by the materialization scheduler.
    if (!guardByPath.count(*canonical))
      guardByPath.emplace(*canonical, std::string());
  }

  std::string restorations;
  for (const auto &entry : guardByPath) {
    if (entry.second.empty())
      continue;
    // The original guard is idempotent, so re-defining it is only safe when it
    // is not already defined; `#ifndef` keeps this exact even if some other path
    // legitimately defined it first.
    restorations += "#ifndef ";
    restorations += entry.second;
    restorations += "\n#define ";
    restorations += entry.second;
    restorations += "\n#endif\n";
    REFOLD_LOG_TRACE("pragma/once/guard",
                     "restoring include guard '{0}' for B-realized header '{1}'",
                     entry.second, entry.first);
  }

  if (restorations.empty())
    return true;

  if (!realizedBody.empty() && realizedBody.back() != '\n')
    realizedBody += "\n";
  realizedBody += restorations;
  return true;
}

std::vector<const PragmaOnceGuard *>
RefoldPragmaOnceGuardRewriter::UsableGuards() const {
  std::vector<const PragmaOnceGuard *> guards;
  for (const auto &entry : candidates_)
    if (entry.second.active && entry.second.guard.IsUsable())
      guards.push_back(&entry.second.guard);
  return guards;
}

//===----------------------------------------------------------------------===//
// Edit staging
//===----------------------------------------------------------------------===//

PragmaOnceGuardEditResult
RefoldPragmaOnceGuardRewriter::StageMaterializedBodyGuardEdits(
    const RefoldModel::IncludeItem &include, StringRef headerPath,
    StringRef headerBytes, std::optional<uint64_t> ancestorArmId,
    std::vector<TextEdit> &edits) const {
  if (!activeSetRecorded_) {
    REFOLD_LOG_FATAL("pragma/once/guard",
                     "guard staging requested before the active header set was "
                     "recorded");
  }

  const PragmaOnceGuard *guard = FindGuardForInclude(include);
  if (!guard) {
    const PragmaOnceGuardRejection rejection = FindRejectionForInclude(include);
    if (rejection == PragmaOnceGuardRejection::None)
      return PragmaOnceGuardEditResult::NotApplicable();
    return PragmaOnceGuardEditResult::Reject(
        rejection,
        formatv("inc#{0} opens a header whose once-state failed closed: {1}",
                include.id, toString(rejection))
            .str());
  }

  // Re-prove the pragma inventory against the bytes actually being emitted.  A
  // caller may preseed a materialized body with text that differs from the file
  // on disk, so catalog offsets are candidates rather than authority here.
  // Re-prove in the domain of the occurrence the producer entered, not this
  // one.  Materializing a *suppressed* occurrence is ordinary -- it is the body
  // the guard makes conditional -- but that occurrence entered nothing, so its
  // own domain carries no pragma record and every once site in it would look
  // unbound.  The inventory belongs to the header's bytes; only the binding
  // evidence belongs to an occurrence, and this names the one that has it.
  const std::optional<uint64_t> bindingOwnerIncludeId =
      ProducerEnteredIncludeIdForPath(guard->physicalHeaderPath)
          .value_or(include.id);

  SmallVector<PragmaOnceSite, 2> sites;
  PragmaOnceGuardRejection rejection = PragmaOnceGuardRejection::None;
  std::string detail;
  if (!DiscoverPragmaOnceSites(guard->physicalHeaderPath, headerPath,
                               bindingOwnerIncludeId, headerBytes, sites,
                               rejection, detail)) {
    return PragmaOnceGuardEditResult::Reject(rejection, std::move(detail));
  }
  if (sites.empty()) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::UnboundPragmaRecord,
        formatv("inc#{0} body for '{1}' contains no once pragma although the "
                "catalog recorded {2}",
                include.id, guard->physicalHeaderPath, guard->sites.size())
            .str());
  }

  const bool emitGuard = guard->treatment == PragmaOnceTreatment::EmitGuard;

  // The `#ifndef`/`#endif` pair adds two physical lines to the body.  With
  // `#line` injection unavailable that drift is permanent, so it is admissible
  // only when nothing in the shifted suffix observes line state.
  //
  // Replacing a `#pragma once` directive is line-neutral on top of that: only
  // the spelling is replaced, so the terminating newline survives and a deleted
  // pragma leaves a blank line behind.  Replacing a `_Pragma("once")` operator
  // that shares its line is not -- it opens a line for the directive -- but the
  // check below already quantifies over the whole body from offset zero,
  // because the prologue shifts every byte of it, and a site's own drift shifts
  // only the suffix after that site.  Both are therefore covered by the same
  // proof.
  if (emitGuard && !GuardLineDriftIsRepairable(include.id, headerPath, 0)) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::UnrepairableLineDrift,
        formatv("inc#{0} guard for '{1}' would shift {2} lines past a preserved "
                "line-state observer with #line injection disabled",
                include.id, guard->physicalHeaderPath, 2)
            .str());
  }

  const AcceptedResultCandidate carrier =
      deps_.proofLattice.AcceptedCandidateBuilder()
          .BuildAcceptedIncludeRealizationCandidate(
              AcceptedPathKind::IncludeMaterializedExpansion, include);

  std::vector<TextEdit> staged;

  // 1) `#ifndef` prologue at the first body byte.  A zero-width insertion whose
  // payload ends in a physical newline keeps any directive that begins the
  // header at logical beginning-of-line.
  if (emitGuard) {
    ResyncOutcome ro = deps_.textEditAssembler.ApplyResyncOrPend(
        headerBytes, 0, 0, (Twine("#ifndef ") + guard->macroName + "\n").str(),
        headerPath, include.id);
    TextEdit edit{0, 0, std::move(ro.text), std::move(ro.pending),
                  std::nullopt, {}, {}, {}};
    edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);
    deps_.textEditAssembler.AttachAcceptedResultCarrier(edit, carrier);
    staged.push_back(std::move(edit));
  }

  // 2) One `#define` per pragma site, replacing the exact directive spelling.
  // Keeping each define at its original site is what makes this form exact for a
  // conditional pragma: a non-firing arm leaves the macro undefined, so a later
  // copy correctly re-emits the body.
  //
  // Several sites in one header all define the same object-like macro with an
  // empty replacement list, which is a benign redefinition under C's identical
  // redefinition rule and therefore warning-free.
  for (const PragmaOnceSite &site : sites) {
    // `DeletePragma` replaces the spelling with nothing.  The pragma is inert in
    // the main file, and removing it keeps `-Wpragma-once-outside-header` out of
    // every refolded TU that inlines a once-header.
    const std::string directive =
        emitGuard ? (Twine("#define ") + guard->macroName).str() : std::string();
    const std::optional<std::string> replacementOrNone =
        OnceSiteInPlaceReplacement(site, headerBytes, directive);
    if (!replacementOrNone) {
      // The site's bytes belong to an enclosing construct.  Declining the
      // in-place rewrite leaves the header's once-state to the B-realized
      // path, which places its define at the top of the body instead.
      return PragmaOnceGuardEditResult::Reject(
          PragmaOnceGuardRejection::PragmaOperatorOnce,
          formatv("inc#{0} once operator at [{1},{2}) in '{3}' is enclosed by "
                  "another protected construct and cannot be rewritten in place",
                  include.id, site.spellingBegin, site.spellingEnd, headerPath)
              .str());
    }
    const std::string &replacement = *replacementOrNone;
    ResyncOutcome ro = deps_.textEditAssembler.ApplyResyncOrPend(
        headerBytes, site.spellingBegin, site.spellingEnd, replacement,
        headerPath, include.id);
    TextEdit edit{site.spellingBegin, site.spellingEnd, std::move(ro.text),
                  std::move(ro.pending), std::nullopt, {}, {}, {}};
    edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);
    if (!deps_.textEditAssembler.AuthorizeExactProtectedSourceInterval(
            edit, ProtectedSourceEditAuthorityKind::PragmaOnceGuardRewrite,
            headerPath, include.id, headerBytes, site.spellingBegin,
            site.spellingEnd, pragmaGuardKinds(), /*allowedNestedKinds=*/{},
            /*requestTerminalOnFailure=*/false)) {
      return PragmaOnceGuardEditResult::Reject(
          PragmaOnceGuardRejection::UnboundPragmaRecord,
          formatv("inc#{0} could not authorize once pragma rewrite at [{1},{2}) "
                  "in '{3}'",
                  include.id, site.spellingBegin, site.spellingEnd, headerPath)
              .str());
    }
    deps_.textEditAssembler.AttachAcceptedResultCarrier(edit, carrier);
    staged.push_back(std::move(edit));
  }

  // 3) `#endif` epilogue after the last body byte.  When the body does not end
  // with a newline the epilogue must start one, otherwise the directive would be
  // appended to a trailing partial line.
  if (emitGuard) {
    const bool needsLeadingNewline =
        !headerBytes.empty() && headerBytes.back() != '\n';
    std::string text =
        (Twine(needsLeadingNewline ? "\n#endif\n" : "#endif\n")).str();
    const uint64_t at = headerBytes.size();
    ResyncOutcome ro = deps_.textEditAssembler.ApplyResyncOrPend(
        headerBytes, at, at, text, headerPath, include.id);
    TextEdit edit{at, at, std::move(ro.text), std::move(ro.pending),
                  std::nullopt, {}, {}, {}};
    edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);
    deps_.textEditAssembler.AttachAcceptedResultCarrier(edit, carrier);
    staged.push_back(std::move(edit));
  }

  edits.insert(edits.end(), std::make_move_iterator(staged.begin()),
               std::make_move_iterator(staged.end()));

  REFOLD_LOG_TRACE("pragma/once/guard",
                   "staged body treatment={0} guard='{1}' inc#{2} path='{3}' "
                   "sites={4}",
                   toString(guard->treatment), guard->macroName, include.id,
                   headerPath, sites.size());
  return PragmaOnceGuardEditResult::Proven();
}

PragmaOnceGuardEditResult
RefoldPragmaOnceGuardRewriter::StageRealizedFromBGuardText(
    const RefoldModel::IncludeItem &include,
    std::optional<uint64_t> ancestorArmId, std::string &realizedBody) const {
  if (!activeSetRecorded_) {
    REFOLD_LOG_FATAL("pragma/once/guard",
                     "guard staging requested before the active header set was "
                     "recorded");
  }

  const PragmaOnceGuard *guard = FindGuardForInclude(include);
  if (!guard) {
    const PragmaOnceGuardRejection rejection = FindRejectionForInclude(include);
    if (rejection == PragmaOnceGuardRejection::None)
      return PragmaOnceGuardEditResult::NotApplicable();
    return PragmaOnceGuardEditResult::Reject(
        rejection,
        formatv("inc#{0} realized from B opens a header whose once-state failed "
                "closed: {1}",
                include.id, toString(rejection))
            .str());
  }

  // A single-occurrence header carries no guard, and a B realization has no
  // pragma text to delete: the tokens never contained the directive.  There is
  // simply nothing to do.
  if (guard->treatment != PragmaOnceTreatment::EmitGuard)
    return PragmaOnceGuardEditResult::NotApplicable();

  // A B realization contains tokens, not header source, so there is no pragma
  // site to rewrite and the define must go at the top of the body.  That is
  // equivalent to the original only when entering the header always fires the
  // pragma, which requires the establishing site to be unconditional through the
  // whole include ancestry and not merely inside this header.
  if (!EstablishingSiteIsUnconditional(guard->sites, ancestorArmId)) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::NonDominatingEstablishingSite,
        formatv("inc#{0} realized from B needs a top-of-body define, but no once "
                "pragma in '{1}' dominates (ancestorArm={2})",
                include.id, guard->physicalHeaderPath,
                ancestorArmId ? std::to_string(*ancestorArmId)
                              : std::string("none"))
            .str());
  }

  // A self-include is the only construct that can observe once-state between the
  // header's first byte and a bottom-of-file pragma site, which is exactly what
  // top-of-body placement would reorder.
  if (HeaderIncludesItself(guard->physicalHeaderPath)) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::SelfIncludingHeader,
        formatv("inc#{0} realized from B opens self-including header '{1}'",
                include.id, guard->physicalHeaderPath)
            .str());
  }

  std::string wrapped;
  wrapped.reserve(realizedBody.size() + 2 * guard->macroName.size() + 32);
  wrapped += "#ifndef ";
  wrapped += guard->macroName;
  wrapped += "\n#define ";
  wrapped += guard->macroName;
  wrapped += "\n";
  wrapped += realizedBody;
  if (!realizedBody.empty() && realizedBody.back() != '\n')
    wrapped += "\n";
  wrapped += "#endif\n";
  realizedBody = std::move(wrapped);

  REFOLD_LOG_TRACE("pragma/once/guard",
                   "wrapped B realization with guard '{0}' for inc#{1}",
                   guard->macroName, include.id);
  return PragmaOnceGuardEditResult::Proven();
}

PragmaOnceGuardEditResult
RefoldPragmaOnceGuardRewriter::StageSurvivingIncludeGuardEdit(
    const RefoldModel::IncludeItem &include, StringRef ownerPath,
    std::optional<uint64_t> ownerIncludeId, StringRef ownerBytes,
    uint64_t siteBegin, uint64_t siteEnd, std::optional<uint64_t> ancestorArmId,
    std::vector<TextEdit> &edits) const {
  if (!activeSetRecorded_) {
    REFOLD_LOG_FATAL("pragma/once/guard",
                     "guard staging requested before the active header set was "
                     "recorded");
  }

  const PragmaOnceGuard *guard = FindGuardForInclude(include);
  if (!guard) {
    // No guard means one of two things, and neither requires an edit here.
    //
    // Either the header establishes no once-state, or its catalog entry failed
    // closed.  A failed entry cannot have been inlined: both inlining paths
    // consult the same catalog, `StageMaterializedBodyGuardEdits()` rejects the
    // source body and `StageRealizedFromBGuardText()` rejects the B realization
    // and requests the terminal result.  So reaching a *surviving* include of a
    // rejected header proves its body is not in the output, which leaves the
    // header's real on-disk `#pragma once` in charge exactly as it was.
    //
    // Rejecting here instead would be actively harmful.  The active-header set
    // is a deliberate over-approximation -- every header reachable from a
    // materialization seed, because the decision to inline is refined during
    // include recursion.  Over-approximating is inert when it merely emits a
    // guard, but treating it as grounds for rejection turns "might be inlined"
    // into a hard failure for headers that were never inlined at all.
    REFOLD_LOG_TRACE("pragma/once/guard",
                     "surviving include inc#{0} needs no guard (rejection={1})",
                     include.id, toString(FindRejectionForInclude(include)));
    return PragmaOnceGuardEditResult::NotApplicable();
  }

  // A surviving include only needs a wrapper when a guard exists.  Reaching here
  // with the delete-only treatment would mean the header has several occurrences
  // after all, so the occurrence count that licensed the deletion was wrong.
  if (guard->treatment != PragmaOnceTreatment::EmitGuard) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::UnaccountedIncludeDirective,
        formatv("surviving include inc#{0} of '{1}' contradicts its "
                "single-occurrence treatment (recorded occurrences={2})",
                include.id, guard->physicalHeaderPath,
                guard->recordedOccurrences)
            .str());
  }

  // The wrapper defines the macro eagerly, before entering the header.  For a
  // conditionally-once header that would mark the header as included even when
  // the original pragma would not have fired, so the establishing site must
  // dominate through the whole include ancestry.
  if (!EstablishingSiteIsUnconditional(guard->sites, ancestorArmId)) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::NonDominatingEstablishingSite,
        formatv("surviving include inc#{0} of '{1}' would need an eager define "
                "but no once pragma there dominates (ancestorArm={2})",
                include.id, guard->physicalHeaderPath,
                ancestorArmId ? std::to_string(*ancestorArmId)
                              : std::string("none"))
            .str());
  }

  if (siteBegin >= siteEnd || siteEnd > ownerBytes.size()) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::IncompleteStructureCensus,
        formatv("surviving include inc#{0} has an invalid site [{1},{2}) in "
                "'{3}' (size {4})",
                include.id, siteBegin, siteEnd, ownerPath, ownerBytes.size())
            .str());
  }

  // The wrapper adds three physical lines around the directive, in TU or
  // parent-header bytes: user source whose suffix may observe line state.
  if (!GuardLineDriftIsRepairable(ownerIncludeId, ownerPath, siteEnd)) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::UnrepairableLineDrift,
        formatv("surviving include inc#{0} wrapper would shift 3 lines past a "
                "preserved line-state observer in '{1}' with #line injection "
                "disabled",
                include.id, ownerPath)
            .str());
  }

  // Preserve the original directive spelling verbatim, including comments,
  // delimiters, macro-computed operands, and `#include_next`.  Only complete
  // physical lines are added around it.
  StringRef original = ownerBytes.substr(siteBegin, siteEnd - siteBegin);
  const bool originalEndsWithNewline = original.ends_with("\n");

  std::string text;
  text.reserve(original.size() + 3 * guard->macroName.size() + 48);
  text += "#ifndef ";
  text += guard->macroName;
  text += "\n#define ";
  text += guard->macroName;
  text += "\n";
  text += original;
  if (!originalEndsWithNewline)
    text += "\n";
  text += "#endif\n";

  ResyncOutcome ro = deps_.textEditAssembler.ApplyResyncOrPend(
      ownerBytes, siteBegin, siteEnd, text, ownerPath, ownerIncludeId);
  TextEdit edit{siteBegin, siteEnd, std::move(ro.text), std::move(ro.pending),
                std::nullopt, {}, {}, {}};
  edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);

  if (!deps_.textEditAssembler.AuthorizeProtectedSourceIntervals(
          edit, ProtectedSourceEditAuthorityKind::PragmaOnceGuardRewrite,
          ownerPath, ownerIncludeId, ownerBytes, siteBegin, siteEnd,
          includeGuardKinds(), /*requireProtectedInterval=*/true,
          /*requestTerminalOnFailure=*/false)) {
    return PragmaOnceGuardEditResult::Reject(
        PragmaOnceGuardRejection::UnauthorizableIncludeSite,
        formatv("surviving include inc#{0} at [{1},{2}) in '{3}' is not an "
                "authorizable include directive for a once guard",
                include.id, siteBegin, siteEnd, ownerPath)
            .str());
  }

  deps_.textEditAssembler.AttachAcceptedResultCarrier(
      edit, deps_.proofLattice.AcceptedCandidateBuilder()
                .BuildAcceptedIncludeRealizationCandidate(
                    AcceptedPathKind::IncludeMaterializedExpansion, include));
  edits.push_back(std::move(edit));

  REFOLD_LOG_TRACE("pragma/once/guard",
                   "staged surviving-include guard '{0}' for inc#{1} at "
                   "[{2},{3}) in '{4}'",
                   guard->macroName, include.id, siteBegin, siteEnd, ownerPath);
  return PragmaOnceGuardEditResult::Proven();
}

} // namespace refold
} // namespace clang

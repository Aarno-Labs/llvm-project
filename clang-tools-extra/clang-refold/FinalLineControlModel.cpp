//===--- FinalLineControlModel.cpp -----------------------------*- C++ -*-===//
//
// Passive final-stream line-control proof scaffolding for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "FinalLineControlModel.h"
#include "RefoldLog.h"
#include "StringUtils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cctype>
#include <limits>

using namespace llvm;

namespace clang {
namespace refold {

StringRef toString(FinalExpectedLineValue::Kind kind) {
  switch (kind) {
  case FinalExpectedLineValue::Kind::Unknown:
    return "Unknown";
  case FinalExpectedLineValue::Kind::Unsigned:
    return "Unsigned";
  case FinalExpectedLineValue::Kind::String:
    return "String";
  }
  llvm_unreachable("invalid final expected line-value kind");
}

StringRef toString(FinalLineDirectiveActivity activity) {
  switch (activity) {
  case FinalLineDirectiveActivity::Unknown:
    return "Unknown";
  case FinalLineDirectiveActivity::KnownInactive:
    return "KnownInactive";
  case FinalLineDirectiveActivity::KnownActive:
    return "KnownActive";
  }
  llvm_unreachable("invalid final line-directive activity");
}

StringRef toString(FinalLineDirectiveSemanticSource source) {
  switch (source) {
  case FinalLineDirectiveSemanticSource::Unknown:
    return "Unknown";
  case FinalLineDirectiveSemanticSource::LiteralFinalSource:
    return "LiteralFinalSource";
  case FinalLineDirectiveSemanticSource::ProducerModel:
    return "ProducerModel";
  }
  llvm_unreachable("invalid final line-directive semantic source");
}

StringRef toString(FinalPhysicalLineKind kind) {
  switch (kind) {
  case FinalPhysicalLineKind::Blank:
    return "Blank";
  case FinalPhysicalLineKind::CommentOnly:
    return "CommentOnly";
  case FinalPhysicalLineKind::Ordinary:
    return "Ordinary";
  case FinalPhysicalLineKind::OtherDirective:
    return "OtherDirective";
  case FinalPhysicalLineKind::ConditionalDirective:
    return "ConditionalDirective";
  case FinalPhysicalLineKind::LineDirective:
    return "LineDirective";
  case FinalPhysicalLineKind::Unknown:
    return "Unknown";
  }
  llvm_unreachable("invalid final physical-line kind");
}

StringRef toString(FinalObserverActivity activity) {
  switch (activity) {
  case FinalObserverActivity::Unknown:
    return "Unknown";
  case FinalObserverActivity::KnownInactive:
    return "KnownInactive";
  case FinalObserverActivity::KnownActive:
    return "KnownActive";
  }
  llvm_unreachable("invalid final observer activity");
}

StringRef toString(FinalObserverSemanticSource source) {
  switch (source) {
  case FinalObserverSemanticSource::Unknown:
    return "Unknown";
  case FinalObserverSemanticSource::LexicalFinalSource:
    return "LexicalFinalSource";
  case FinalObserverSemanticSource::ProducerModel:
    return "ProducerModel";
  }
  llvm_unreachable("invalid final observer semantic source");
}

StringRef toString(FinalLineDirective::Origin origin) {
  switch (origin) {
  case FinalLineDirective::Origin::PreservedSource:
    return "PreservedSource";
  case FinalLineDirective::Origin::SyntheticIncludeEntry:
    return "SyntheticIncludeEntry";
  case FinalLineDirective::Origin::SyntheticIncludeReturn:
    return "SyntheticIncludeReturn";
  case FinalLineDirective::Origin::SyntheticNewlineResync:
    return "SyntheticNewlineResync";
  case FinalLineDirective::Origin::SyntheticSourceLineResume:
    return "SyntheticSourceLineResume";
  case FinalLineDirective::Origin::SyntheticTUPrologue:
    return "SyntheticTUPrologue";
  case FinalLineDirective::Origin::SyntheticLayoutBarrier:
    return "SyntheticLayoutBarrier";
  case FinalLineDirective::Origin::Unknown:
    return "Unknown";
  }
  llvm_unreachable("invalid final line-directive origin");
}

StringRef toString(FinalObserver::Kind kind) {
  switch (kind) {
  case FinalObserver::Kind::Line:
    return "Line";
  case FinalObserver::Kind::File:
    return "File";
  case FinalObserver::Kind::FileName:
    return "FileName";
  }
  llvm_unreachable("invalid final observer kind");
}

StringRef toString(FinalLineObserverComponent component) {
  switch (component) {
  case FinalLineObserverComponent::Line:
    return "Line";
  case FinalLineObserverComponent::File:
    return "File";
  case FinalLineObserverComponent::FileName:
    return "FileName";
  }
  llvm_unreachable("invalid final line-observer component");
}

StringRef toString(FinalLayoutObligation::Kind kind) {
  switch (kind) {
  case FinalLayoutObligation::Kind::ZeroTokenPrefixBarrier:
    return "ZeroTokenPrefixBarrier";
  case FinalLayoutObligation::Kind::ZeroTokenGapBarrier:
    return "ZeroTokenGapBarrier";
  case FinalLayoutObligation::Kind::FirstVisibleTokenAlignment:
    return "FirstVisibleTokenAlignment";
  case FinalLayoutObligation::Kind::BlankLinePreservation:
    return "BlankLinePreservation";
  }
  llvm_unreachable("invalid final layout-obligation kind");
}

std::string FinalLineControlOwnerKey::ToString() const {
  return formatv("owner(file='{0}', include={1})", physicalFile,
                 ownerIncludeId)
      .str();
}


std::string FinalLineControlSourceMapping::ToString() const {
  return formatv("source-map(final=[{0},{1}) source='{2}'[{3},{4}) include={5})",
                 finalBegin, finalEnd, physicalFile, sourceBegin, sourceEnd,
                 ownerIncludeId)
      .str();
}


void CanonicalizeFinalLineControlSourceMappings(
    std::vector<FinalLineControlSourceMapping> &mappings) {
  mappings.erase(
      std::remove_if(mappings.begin(), mappings.end(),
                     [](const FinalLineControlSourceMapping &mapping) {
                       return mapping.finalBegin >= mapping.finalEnd ||
                              mapping.sourceBegin >= mapping.sourceEnd;
                     }),
      mappings.end());

  llvm::sort(mappings, [](const FinalLineControlSourceMapping &lhs,
                          const FinalLineControlSourceMapping &rhs) {
    if (lhs.finalBegin != rhs.finalBegin)
      return lhs.finalBegin < rhs.finalBegin;
    if (lhs.finalEnd != rhs.finalEnd)
      return lhs.finalEnd < rhs.finalEnd;
    if (lhs.physicalFile != rhs.physicalFile)
      return lhs.physicalFile < rhs.physicalFile;
    if (lhs.ownerIncludeId != rhs.ownerIncludeId)
      return lhs.ownerIncludeId < rhs.ownerIncludeId;
    if (lhs.sourceBegin != rhs.sourceBegin)
      return lhs.sourceBegin < rhs.sourceBegin;
    return lhs.sourceEnd < rhs.sourceEnd;
  });

  std::vector<FinalLineControlSourceMapping> canonical;
  canonical.reserve(mappings.size());
  for (FinalLineControlSourceMapping mapping : mappings) {
    if (!canonical.empty()) {
      FinalLineControlSourceMapping &prev = canonical.back();
      if (prev.finalBegin == mapping.finalBegin &&
          prev.finalEnd == mapping.finalEnd &&
          prev.physicalFile == mapping.physicalFile &&
          prev.ownerIncludeId == mapping.ownerIncludeId &&
          prev.sourceBegin == mapping.sourceBegin &&
          prev.sourceEnd == mapping.sourceEnd) {
        continue;
      }

      if (prev.finalEnd == mapping.finalBegin &&
          prev.sourceEnd == mapping.sourceBegin &&
          prev.physicalFile == mapping.physicalFile &&
          prev.ownerIncludeId == mapping.ownerIncludeId) {
        prev.finalEnd = mapping.finalEnd;
        prev.sourceEnd = mapping.sourceEnd;
        continue;
      }
    }

    canonical.push_back(std::move(mapping));
  }

  mappings = std::move(canonical);
}

void AdjustFinalLineControlSourceMappingsAfterDeletion(
    std::vector<FinalLineControlSourceMapping> &mappings, uint64_t removedBegin,
    uint64_t removedEnd) {
  if (removedBegin >= removedEnd)
    return;

  const uint64_t removedSize = removedEnd - removedBegin;
  std::vector<FinalLineControlSourceMapping> adjusted;
  adjusted.reserve(mappings.size() + 1);

  for (FinalLineControlSourceMapping mapping : mappings) {
    if (mapping.finalEnd <= removedBegin) {
      adjusted.push_back(std::move(mapping));
      continue;
    }

    if (mapping.finalBegin >= removedEnd) {
      mapping.finalBegin -= removedSize;
      mapping.finalEnd -= removedSize;
      adjusted.push_back(std::move(mapping));
      continue;
    }

    // The deleted bytes intersect this copied-source span.  Preserve exact
    // provenance for the surviving prefix and suffix, but deliberately leave no
    // mapping across the deleted hole.
    if (mapping.finalBegin < removedBegin) {
      FinalLineControlSourceMapping prefix = mapping;
      prefix.finalEnd = removedBegin;
      prefix.sourceEnd = mapping.sourceBegin + (removedBegin - mapping.finalBegin);
      adjusted.push_back(std::move(prefix));
    }

    if (removedEnd < mapping.finalEnd) {
      FinalLineControlSourceMapping suffix = mapping;
      suffix.sourceBegin = mapping.sourceBegin + (removedEnd - mapping.finalBegin);
      suffix.finalBegin = removedBegin;
      suffix.finalEnd = mapping.finalEnd - removedSize;
      adjusted.push_back(std::move(suffix));
    }
  }

  mappings = std::move(adjusted);
  CanonicalizeFinalLineControlSourceMappings(mappings);
}


/// Canonicalize removable final-line-control candidates so the fixed-point
/// pruner is independent of incidental emitter/vector ordering.
///
/// Multiple local emission paths can conservatively describe the same final
/// `#line` byte range.  The physical pruning decision depends only on that
/// range plus the final-stream liveness proofs; origin/owner/reason fields are
/// trace/provenance annotations.  Merge exact-range duplicates deterministically
/// before scanning so a candidate's annotations cannot depend on append order.
static void CanonicalizeFinalLineControlPruneCandidates(
    std::vector<FinalLineControlPruneCandidate> &candidates) {
  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(),
                     [](const FinalLineControlPruneCandidate &candidate) {
                       return candidate.finalBegin >= candidate.finalEnd;
                     }),
      candidates.end());

  auto ownerLess = [](const std::optional<FinalLineControlOwnerKey> &lhs,
                       const std::optional<FinalLineControlOwnerKey> &rhs) {
    if (lhs.has_value() != rhs.has_value())
      return !lhs.has_value();
    if (!lhs)
      return false;
    if (lhs->physicalFile != rhs->physicalFile)
      return lhs->physicalFile < rhs->physicalFile;
    return lhs->ownerIncludeId < rhs->ownerIncludeId;
  };

  llvm::sort(candidates, [&](const FinalLineControlPruneCandidate &lhs,
                             const FinalLineControlPruneCandidate &rhs) {
    if (lhs.finalBegin != rhs.finalBegin)
      return lhs.finalBegin < rhs.finalBegin;
    if (lhs.finalEnd != rhs.finalEnd)
      return lhs.finalEnd < rhs.finalEnd;
    if (lhs.origin != rhs.origin)
      return static_cast<unsigned>(lhs.origin) < static_cast<unsigned>(rhs.origin);
    if (ownerLess(lhs.physicalOwner, rhs.physicalOwner))
      return true;
    if (ownerLess(rhs.physicalOwner, lhs.physicalOwner))
      return false;
    if (lhs.producerProven != rhs.producerProven)
      return !lhs.producerProven && rhs.producerProven;
    return lhs.reason < rhs.reason;
  });

  auto sameOwner = [](const std::optional<FinalLineControlOwnerKey> &lhs,
                      const std::optional<FinalLineControlOwnerKey> &rhs) {
    if (lhs.has_value() != rhs.has_value())
      return false;
    if (!lhs)
      return true;
    return lhs->physicalFile == rhs->physicalFile &&
           lhs->ownerIncludeId == rhs->ownerIncludeId;
  };

  auto appendUniqueReason = [](std::string &dst, StringRef reason) {
    if (reason.empty())
      return;
    if (dst.empty()) {
      dst = reason.str();
      return;
    }
    // Reasons are diagnostics only.  A substring check avoids repeatedly
    // appending identical text without adding order-sensitive containers here.
    if (StringRef(dst).contains(reason))
      return;
    dst += "; ";
    dst += reason.str();
  };

  std::vector<FinalLineControlPruneCandidate> canonical;
  canonical.reserve(candidates.size());
  for (FinalLineControlPruneCandidate candidate : candidates) {
    if (!canonical.empty() &&
        canonical.back().finalBegin == candidate.finalBegin &&
        canonical.back().finalEnd == candidate.finalEnd) {
      FinalLineControlPruneCandidate &merged = canonical.back();

      if (merged.origin != candidate.origin)
        merged.origin = FinalLineDirective::Origin::Unknown;

      if (!sameOwner(merged.physicalOwner, candidate.physicalOwner))
        merged.physicalOwner = std::nullopt;

      merged.producerProven = merged.producerProven || candidate.producerProven;
      appendUniqueReason(merged.reason, candidate.reason);
      continue;
    }

    canonical.push_back(std::move(candidate));
  }

  candidates = std::move(canonical);
}

std::string FinalLineControlProducerEvent::ToString() const {
  return formatv("producer-line-event#{0} source='{1}' site=[{2},{3}) active={4} "
                 "proven={5} logical=({6},'{7}') include={8} text='{9}'",
                 id, physicalFile, siteBegin, siteEnd, active, producerProven,
                 logicalLineAfter, stringutils::showWsWithClip(logicalFileAfter, 80),
                 ownerIncludeId, stringutils::showWsWithClip(text, 160))
      .str();
}

std::string FinalExpectedLineValue::ToString() const {
  switch (kind) {
  case Kind::Unknown:
    return "Unknown";
  case Kind::Unsigned:
    return formatv("Unsigned({0})", unsignedValue).str();
  case Kind::String:
    return formatv("String('{0}')", stringutils::showWsWithClip(stringValue, 80))
        .str();
  }
  llvm_unreachable("invalid final expected line-value kind");
}

std::string FinalLogicalState::ToString() const {
  return formatv("state(line={0}, file={1})",
                 lineKnown ? std::to_string(logicalLine) : std::string("unknown"),
                 fileKnown ? stringutils::showWsWithClip(logicalFile, 80)
                           : std::string("unknown"))
      .str();
}

std::string FinalPhysicalLine::ToString() const {
  return formatv(
             "line#{0} [{1},{2}) kind={3} directiveName='{4}' "
             "condDepth={5}->{6} before={7} after={8} directive={9} "
             "text='{10}'",
             physicalLine, finalBegin, finalEnd, kind, directiveName,
             conditionalDepthBefore, conditionalDepthAfter, stateBefore,
             stateAfter, directiveIndex,
             stringutils::showWsWithClip(rawText, 160))
      .str();
}

std::string FinalLineDirective::ToString() const {
  return formatv(
             "directive[{0},{1}) origin={2} activity={3} source={4} "
             "semanticsKnown={5} line={6} file={7} owner={8} "
             "producerProven={9} producerEvent={10} removable={11} text='{12}'",
             finalBegin, finalEnd, origin, activity, semanticSource,
             semanticsKnown, line, file, physicalOwner, producerProven,
             producerEventId, removable, stringutils::showWsWithClip(rawText, 160))
      .str();
}

std::string FinalObserver::ToString() const {
  return formatv(
             "observer[{0},{1}) line#{2} kind={3} activity={4} source={5} "
             "expected={6} owner={7} producerProven={8} spellingPreserved={9} "
             "before={10} text='{11}'",
             finalOffset, finalEnd, physicalLine, kind, activity, semanticSource,
             expected, physicalOwner, producerProven, spellingPreserved,
             stateBefore, stringutils::showWsWithClip(rawText, 80))
      .str();
}

std::string FinalLineControlProducerObserver::ToString() const {
  return formatv(
             "producer-observer#{0} kind={1} source='{2}'[{3},{4}) "
             "include={5} active={6} proven={7} expected={8} text='{9}'",
             id, kind, physicalFile, sourceBegin, sourceEnd, ownerIncludeId,
             active, producerProven, expected,
             stringutils::showWsWithClip(text, 160))
      .str();
}

std::string FinalLineDirectiveObserverLiveness::ToString() const {
  std::string joinedReasons;
  for (size_t i = 0; i < reasons.size(); ++i) {
    if (i)
      joinedReasons += "; ";
    joinedReasons += reasons[i];
  }
  return formatv(
             "observer-live directive#{0} [{1},{2}) live(line={3}, file={4}, "
             "filename={5}) unknown(line={6}, file={7}, filename={8}) "
             "observerDead={9} removableIfLayoutDead={10} reasons='{11}'",
             directiveIndex, finalBegin, finalEnd, lineLive, fileLive,
             fileNameLive, lineUnknownDependence, fileUnknownDependence,
             fileNameUnknownDependence, observerDead, removableIfLayoutDead,
             stringutils::showWsWithClip(joinedReasons, 240))
      .str();
}

std::string FinalLayoutObligation::ToString() const {
  return formatv("layout@{0} kind={1} directive={2} owner={3} reason='{4}'",
                 finalOffset, kind, directiveIndex, physicalOwner,
                 stringutils::showWsWithClip(reason, 160))
      .str();
}

std::string FinalLineDirectiveLayoutLiveness::ToString() const {
  std::string joinedReasons;
  for (size_t i = 0; i < reasons.size(); ++i) {
    if (i)
      joinedReasons += "; ";
    joinedReasons += reasons[i];
  }

  return formatv(
             "layout-live directive#{0} [{1},{2}) live(prefix={3}, gap={4}, "
             "firstVisible={5}, blank={6}) unknown={7} layoutDead={8} "
             "removableIfObserverDead={9} reasons='{10}'",
             directiveIndex, finalBegin, finalEnd, zeroTokenPrefixBarrierLive,
             zeroTokenGapBarrierLive, firstVisibleTokenAlignmentLive,
             blankLinePreservationLive, layoutUnknownDependence, layoutDead,
             removableIfObserverDead,
             stringutils::showWsWithClip(joinedReasons, 240))
      .str();
}


std::string FinalLineControlPruneCandidate::ToString() const {
  return formatv(
             "prune-candidate [{0},{1}) origin={2} owner={3} "
             "producerProven={4} reason='{5}'",
             finalBegin, finalEnd, origin, physicalOwner, producerProven,
             stringutils::showWsWithClip(reason, 160))
      .str();
}

std::string FinalLineControlPruneDecision::ToString() const {
  std::string joinedReasons;
  for (size_t i = 0; i < reasons.size(); ++i) {
    if (i)
      joinedReasons += "; ";
    joinedReasons += reasons[i];
  }

  return formatv(
             "prune-decision iter={0} directive#{1} [{2},{3}) "
             "candidate={4} observerDead={5} layoutDead={6} "
             "verificationAttempted={7} verificationRejected={8} "
             "removed={9} reasons='{10}'",
             iteration, directiveIndex, finalBegin, finalEnd, explicitCandidate,
             observerDead, layoutDead, verificationAttempted,
             verificationRejected, removed,
             stringutils::showWsWithClip(joinedReasons, 240))
      .str();
}

namespace {

enum class DirectiveKind : uint8_t {
  None,
  Other,
  ConditionalEnter,
  ConditionalMiddle,
  ConditionalExit,
  LineControl,
};

struct DirectiveInfo {
  DirectiveKind kind = DirectiveKind::None;
  size_t payloadBegin = 0;
  StringRef name;
};

struct ParsedLineControl {
  bool recognized = false;
  bool semanticsKnown = false;
  bool hashNumberForm = false;
  std::optional<uint64_t> line;
  std::optional<std::string> file;
};


void applyMatchingPruneCandidate(
    FinalLineDirective &directive,
    ArrayRef<FinalLineControlPruneCandidate> removableCandidates) {
  for (const FinalLineControlPruneCandidate &candidate : removableCandidates) {
    if (candidate.finalBegin != directive.finalBegin ||
        candidate.finalEnd != directive.finalEnd)
      continue;

    directive.removable = true;
    directive.origin = candidate.origin;
    directive.physicalOwner = candidate.physicalOwner;
    directive.producerProven = candidate.producerProven;
    return;
  }
}


std::optional<FinalLineControlSourceMapping> mapFinalDirectiveToSource(
    uint64_t finalBegin, uint64_t finalEnd,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings) {
  for (const FinalLineControlSourceMapping &mapping : sourceMappings) {
    if (finalBegin < mapping.finalBegin || finalEnd > mapping.finalEnd)
      continue;
    const uint64_t deltaBegin = finalBegin - mapping.finalBegin;
    const uint64_t deltaEnd = finalEnd - mapping.finalBegin;
    const uint64_t sourceBegin = mapping.sourceBegin + deltaBegin;
    const uint64_t sourceEnd = mapping.sourceBegin + deltaEnd;
    if (sourceEnd > mapping.sourceEnd)
      continue;

    FinalLineControlSourceMapping narrowed = mapping;
    narrowed.finalBegin = finalBegin;
    narrowed.finalEnd = finalEnd;
    narrowed.sourceBegin = sourceBegin;
    narrowed.sourceEnd = sourceEnd;
    return narrowed;
  }
  return std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>> mapSourceRangeToFinal(
    StringRef finalSource, const FinalLineControlProducerObserver &observer,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings) {
  if (!observer.producerProven || !observer.active)
    return std::nullopt;
  if (observer.sourceEnd <= observer.sourceBegin)
    return std::nullopt;

  for (const FinalLineControlSourceMapping &mapping : sourceMappings) {
    if (mapping.physicalFile != observer.physicalFile)
      continue;
    if (mapping.ownerIncludeId != observer.ownerIncludeId)
      continue;
    if (observer.sourceBegin < mapping.sourceBegin ||
        observer.sourceEnd > mapping.sourceEnd)
      continue;

    const uint64_t finalBegin =
        mapping.finalBegin + (observer.sourceBegin - mapping.sourceBegin);
    const uint64_t finalEnd =
        mapping.finalBegin + (observer.sourceEnd - mapping.sourceBegin);
    if (finalEnd > mapping.finalEnd || finalEnd > finalSource.size())
      continue;

    if (!observer.text.empty() &&
        finalSource.slice(finalBegin, finalEnd) != StringRef(observer.text))
      continue;

    return std::make_pair(finalBegin, finalEnd);
  }

  return std::nullopt;
}

void collectProducerObserverCandidatesForLine(
    FinalLineControlModel &model, StringRef finalSource, uint64_t lineBegin,
    uint64_t lineEnd, uint64_t physicalLineNo,
    const FinalLogicalState &stateBefore,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings,
    ArrayRef<FinalLineControlProducerObserver> producerObservers) {
  for (const FinalLineControlProducerObserver &producerObserver :
       producerObservers) {
    std::optional<std::pair<uint64_t, uint64_t>> finalRange =
        mapSourceRangeToFinal(finalSource, producerObserver, sourceMappings);
    if (!finalRange)
      continue;

    const uint64_t finalBegin = finalRange->first;
    const uint64_t finalEnd = finalRange->second;
    if (finalBegin < lineBegin || finalBegin >= lineEnd)
      continue;

    FinalObserver observer;
    observer.finalOffset = finalBegin;
    observer.finalEnd = finalEnd;
    observer.physicalLine = physicalLineNo;
    observer.kind = producerObserver.kind;
    observer.expected = producerObserver.expected;
    observer.physicalOwner = FinalLineControlOwnerKey(
        producerObserver.physicalFile, producerObserver.ownerIncludeId);
    observer.activity = producerObserver.active
                            ? FinalObserverActivity::KnownActive
                            : FinalObserverActivity::KnownInactive;
    observer.semanticSource = FinalObserverSemanticSource::ProducerModel;
    observer.producerProven = producerObserver.producerProven;
    observer.spellingPreserved = producerObserver.producerProven;
    observer.stateBefore = stateBefore;
    observer.rawText = producerObserver.text.empty()
                           ? finalSource.slice(finalBegin, finalEnd).str()
                           : producerObserver.text;
    model.AddObserver(std::move(observer));
  }
}

bool producerEventMatchesParsedLiteralState(
    const FinalLineControlProducerEvent &event,
    const ParsedLineControl &parsedLineControl) {
  // When the final source spelling is a literal #line directive, use that
  // literal post-state to disambiguate producer records for the same physical
  // site.  The producer may record more than one event for one directive site
  // (for example, pre-transition bookkeeping plus the actual post-directive
  // state).  Binding the final directive to the first same-site/same-text event
  // can therefore assign stale state such as the pre-directive file/line.
  //
  // If the final spelling is not literal enough for the scanner to know the
  // post-state, do not filter here: macro-expanded #line operands must continue
  // to rely on the producer model.
  if (!parsedLineControl.semanticsKnown)
    return true;
  if (parsedLineControl.line &&
      event.logicalLineAfter != *parsedLineControl.line)
    return false;
  if (parsedLineControl.file &&
      event.logicalFileAfter != *parsedLineControl.file)
    return false;
  return true;
}

const FinalLineControlProducerEvent *findProducerLineControlEvent(
    const FinalLineControlSourceMapping &sourceMapping,
    StringRef finalDirectiveText, const ParsedLineControl &parsedLineControl,
    ArrayRef<FinalLineControlProducerEvent> producerEvents) {
  for (const FinalLineControlProducerEvent &event : producerEvents) {
    if (!event.producerProven || !event.active)
      continue;
    if (!event.siteBegin || !event.siteEnd)
      continue;
    if (event.physicalFile != sourceMapping.physicalFile)
      continue;
    if (event.ownerIncludeId != sourceMapping.ownerIncludeId)
      continue;
    if (*event.siteBegin != sourceMapping.sourceBegin ||
        *event.siteEnd != sourceMapping.sourceEnd)
      continue;
    if (!event.text.empty() && StringRef(event.text) != finalDirectiveText)
      continue;
    if (!producerEventMatchesParsedLiteralState(event, parsedLineControl))
      continue;
    return &event;
  }
  return nullptr;
}

void applyProducerLineControlEvent(
    FinalLineDirective &directive, const FinalLineControlSourceMapping &mapping,
    const FinalLineControlProducerEvent &event) {
  directive.activity = event.active ? FinalLineDirectiveActivity::KnownActive
                                    : FinalLineDirectiveActivity::KnownInactive;
  directive.semanticSource = FinalLineDirectiveSemanticSource::ProducerModel;
  directive.semanticsKnown = event.producerProven;
  directive.producerProven = event.producerProven;
  directive.producerEventId = event.id;
  directive.physicalOwner = FinalLineControlOwnerKey(mapping.physicalFile,
                                                     mapping.ownerIncludeId);
  if (event.producerProven) {
    directive.line = event.logicalLineAfter;
    directive.file = event.logicalFileAfter;
  }
}

/// Mark a source-authored final-stream line-control directive as producer-proven.
///
/// Source-authored `#line` directives are source material, not minimization
/// artifacts.  Once Step #2's exact final-to-source mapping has matched the
/// final directive spelling to a producer LineControlEvent for the same
/// physical directive bytes, record the provenance and known post-state, but do
/// not make the directive removable.  A later synthesized repair may dominate
/// all live observers, but that does not make explicitly authored source text
/// gratuitous under the preservation policy.
void markProducerProvenSourceLineControlPreserved(
    FinalLineDirective &directive, const FinalLineControlSourceMapping &mapping,
    const FinalLineControlProducerEvent &event) {
  if (!event.producerProven || !event.active)
    return;
  if (!directive.semanticsKnown ||
      directive.semanticSource != FinalLineDirectiveSemanticSource::ProducerModel)
    return;
  if (directive.activity != FinalLineDirectiveActivity::KnownActive)
    return;
  if (!directive.line)
    return;

  directive.origin = FinalLineDirective::Origin::PreservedSource;
  directive.removable = false;
  directive.producerProven = true;
  directive.physicalOwner = FinalLineControlOwnerKey(mapping.physicalFile,
                                                     mapping.ownerIncludeId);
}

bool isHorizontalWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

bool isDecimalDigit(char c) { return c >= '0' && c <= '9'; }

bool isIdentifierStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentifierContinue(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

void skipHorizontalWhitespace(StringRef line, size_t &i) {
  while (i < line.size() && isHorizontalWhitespace(line[i]))
    ++i;
}

bool physicalLineContinuesWithBackslash(StringRef line) {
  // The scanner works on physical source lines with the terminating '\n'
  // removed.  For CRLF input, the remaining '\r' is part of the line ending,
  // not source spelling for this purpose.  Do not trim spaces or tabs: C line
  // splicing requires the backslash to be immediately before the physical
  // newline, and treating "\\   \n" as a continuation would make a directive
  // segment proof depend on non-standard recovery behavior.
  if (line.ends_with("\r"))
    line = line.drop_back();
  return line.ends_with("\\");
}

/// Skip whitespace/comments between preprocessing tokens on one final physical
/// line.  A line comment consumes the rest of the physical line.  An unclosed
/// block comment leaves the caller with no known following token on this line.
bool skipDirectiveTokenWhitespace(StringRef line, size_t &i) {
  for (;;) {
    skipHorizontalWhitespace(line, i);
    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') {
      i = line.size();
      return true;
    }
    if (i + 1 >= line.size() || line[i] != '/' || line[i + 1] != '*')
      return true;

    size_t close = line.find("*/", i + 2);
    if (close == StringRef::npos) {
      i = line.size();
      return false;
    }
    i = close + 2;
  }
}

/// Locate a directive introducer after replacing comments by whitespace, while
/// carrying block-comment state across final physical lines.
bool locateDirectiveHash(StringRef line, bool &inBlockComment, size_t &hash) {
  size_t i = 0;
  for (;;) {
    if (inBlockComment) {
      size_t close = line.find("*/", i);
      if (close == StringRef::npos)
        return false;
      i = close + 2;
      inBlockComment = false;
      continue;
    }

    skipHorizontalWhitespace(line, i);
    if (i >= line.size())
      return false;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
      return false;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
      size_t close = line.find("*/", i + 2);
      if (close == StringRef::npos) {
        inBlockComment = true;
        return false;
      }
      i = close + 2;
      continue;
    }

    if (line[i] != '#')
      return false;

    hash = i;
    return true;
  }
}

/// Classify lines that have no directive introducer.  This only needs enough
/// precision for Chunk 2 trace output; final layout liveness is handled later by
/// a dedicated proof pass.
FinalPhysicalLineKind classifyNonDirectiveLine(StringRef line,
                                               bool startsInBlockComment) {
  size_t i = 0;
  bool inBlockComment = startsInBlockComment;
  bool sawComment = startsInBlockComment;
  for (;;) {
    if (inBlockComment) {
      size_t close = line.find("*/", i);
      if (close == StringRef::npos)
        return FinalPhysicalLineKind::CommentOnly;
      i = close + 2;
      inBlockComment = false;
      continue;
    }

    skipHorizontalWhitespace(line, i);
    if (i >= line.size())
      return sawComment ? FinalPhysicalLineKind::CommentOnly
                        : FinalPhysicalLineKind::Blank;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
      return FinalPhysicalLineKind::CommentOnly;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
      sawComment = true;
      size_t close = line.find("*/", i + 2);
      if (close == StringRef::npos)
        return FinalPhysicalLineKind::CommentOnly;
      i = close + 2;
      continue;
    }

    return FinalPhysicalLineKind::Ordinary;
  }
}

DirectiveInfo classifyDirective(StringRef line, size_t hash) {
  DirectiveInfo out;
  size_t i = hash + 1;
  skipHorizontalWhitespace(line, i);

  if (i >= line.size()) {
    out.kind = DirectiveKind::Other;
    out.payloadBegin = i;
    return out;
  }

  if (isDecimalDigit(line[i])) {
    out.kind = DirectiveKind::LineControl;
    out.payloadBegin = i;
    return out;
  }

  if (!isIdentifierStart(line[i])) {
    out.kind = DirectiveKind::Other;
    out.payloadBegin = i;
    return out;
  }

  const size_t nameBegin = i;
  while (i < line.size() && isIdentifierContinue(line[i]))
    ++i;
  out.name = line.slice(nameBegin, i);
  out.payloadBegin = i;

  if (out.name == "line")
    out.kind = DirectiveKind::LineControl;
  else if (out.name == "if" || out.name == "ifdef" || out.name == "ifndef")
    out.kind = DirectiveKind::ConditionalEnter;
  else if (out.name == "elif" || out.name == "elifdef" ||
           out.name == "elifndef" || out.name == "else")
    out.kind = DirectiveKind::ConditionalMiddle;
  else if (out.name == "endif")
    out.kind = DirectiveKind::ConditionalExit;
  else
    out.kind = DirectiveKind::Other;

  return out;
}

bool parseUnsignedDecimal(StringRef line, size_t &i, uint64_t &value) {
  skipDirectiveTokenWhitespace(line, i);
  if (i >= line.size() || !isDecimalDigit(line[i]))
    return false;

  uint64_t result = 0;
  do {
    const uint64_t digit = static_cast<uint64_t>(line[i] - '0');
    if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10)
      return false;
    result = result * 10 + digit;
    ++i;
  } while (i < line.size() && isDecimalDigit(line[i]));

  value = result;
  return true;
}

/// Parse an ordinary narrow string-literal token and return its raw spelling
/// without the surrounding quotes.  Escape decoding is intentionally deferred to
/// producer/model support; if a future pruning proof needs exact `__FILE__`
/// semantics for escaped filenames, it should consume the producer-proven line
/// control event rather than guessing here.
bool parseRawOrdinaryStringLiteral(StringRef line, size_t &i,
                                   std::string &value) {
  skipDirectiveTokenWhitespace(line, i);
  if (i >= line.size() || line[i] != '"')
    return false;

  const size_t contentBegin = i + 1;
  ++i;
  bool escaped = false;
  while (i < line.size()) {
    const char c = line[i];
    if (escaped) {
      escaped = false;
      ++i;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      ++i;
      continue;
    }
    if (c == '"') {
      value = line.slice(contentBegin, i).str();
      ++i;
      return true;
    }
    ++i;
  }

  return false;
}

bool skipTrailingIgnorable(StringRef line, size_t &i) {
  for (;;) {
    skipHorizontalWhitespace(line, i);
    if (i >= line.size())
      return true;
    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') {
      i = line.size();
      return true;
    }
    if (i + 1 >= line.size() || line[i] != '/' || line[i + 1] != '*')
      return false;
    size_t close = line.find("*/", i + 2);
    if (close == StringRef::npos)
      return false;
    i = close + 2;
  }
}

bool skipHashNumberLineMarkerFlags(StringRef line, size_t &i) {
  for (;;) {
    const size_t beforeWhitespace = i;
    if (!skipDirectiveTokenWhitespace(line, i))
      return false;
    if (i >= line.size())
      return true;
    if (!isDecimalDigit(line[i])) {
      i = beforeWhitespace;
      return skipTrailingIgnorable(line, i);
    }

    uint64_t ignored = 0;
    if (!parseUnsignedDecimal(line, i, ignored))
      return false;
  }
}

ParsedLineControl parseLineControl(StringRef line, const DirectiveInfo &info) {
  ParsedLineControl out;
  out.recognized = true;

  size_t i = info.payloadBegin;
  uint64_t parsedLine = 0;
  if (!parseUnsignedDecimal(line, i, parsedLine))
    return out;

  out.line = parsedLine;

  std::optional<std::string> parsedFile;
  size_t beforeOptionalFile = i;
  if (!skipDirectiveTokenWhitespace(line, i))
    return out;
  if (i < line.size() && line[i] == '"') {
    std::string file;
    if (!parseRawOrdinaryStringLiteral(line, i, file))
      return out;
    parsedFile = std::move(file);
  } else {
    i = beforeOptionalFile;
  }

  out.file = std::move(parsedFile);
  out.hashNumberForm = info.name.empty();
  if (out.hashNumberForm)
    out.semanticsKnown = skipHashNumberLineMarkerFlags(line, i);
  else
    out.semanticsKnown = skipTrailingIgnorable(line, i);
  return out;
}

FinalPhysicalLineKind physicalKindForDirective(DirectiveKind kind) {
  switch (kind) {
  case DirectiveKind::None:
    return FinalPhysicalLineKind::Unknown;
  case DirectiveKind::Other:
    return FinalPhysicalLineKind::OtherDirective;
  case DirectiveKind::ConditionalEnter:
  case DirectiveKind::ConditionalMiddle:
  case DirectiveKind::ConditionalExit:
    return FinalPhysicalLineKind::ConditionalDirective;
  case DirectiveKind::LineControl:
    return FinalPhysicalLineKind::LineDirective;
  }
  llvm_unreachable("invalid directive kind");
}

bool directiveMayChangeFile(const ParsedLineControl &parsed) {
  return !parsed.semanticsKnown || parsed.file.has_value();
}

std::optional<FinalObserver::Kind> observerKindForIdentifier(StringRef ident) {
  if (ident == "__LINE__")
    return FinalObserver::Kind::Line;
  if (ident == "__FILE__")
    return FinalObserver::Kind::File;
  if (ident == "__FILE_NAME__")
    return FinalObserver::Kind::FileName;
  return std::nullopt;
}

bool skipEscapedQuotedLiteral(StringRef line, size_t &i, char quote) {
  if (i >= line.size() || line[i] != quote)
    return false;

  ++i;
  bool escaped = false;
  while (i < line.size()) {
    const char c = line[i++];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == quote)
      return true;
  }

  return true;
}

bool skipOrdinaryStringOrCharLiteral(StringRef line, size_t &i) {
  static const StringRef Prefixes[] = {
      "u8\"", "u8'", "u\"", "u'", "U\"", "U'", "L\"", "L'", "\"", "'"};

  for (StringRef prefix : Prefixes) {
    if (!line.substr(i).starts_with(prefix))
      continue;
    size_t quote = i + prefix.size() - 1;
    const char quoteChar = line[quote];
    i = quote;
    return skipEscapedQuotedLiteral(line, i, quoteChar);
  }

  return false;
}

bool skipRawStringLiteral(StringRef line, size_t &i) {
  static const StringRef Prefixes[] = {"u8R\"", "uR\"", "UR\"",
                                      "LR\"", "R\""};

  for (StringRef prefix : Prefixes) {
    if (!line.substr(i).starts_with(prefix))
      continue;

    const size_t delimiterBegin = i + prefix.size();
    size_t openParen = delimiterBegin;
    while (openParen < line.size() && line[openParen] != '(') {
      const char c = line[openParen];
      if (isHorizontalWhitespace(c) || c == ')' || c == '\\')
        return false;
      ++openParen;
    }
    if (openParen >= line.size()) {
      i = line.size();
      return true;
    }

    const std::string delimiter = line.slice(delimiterBegin, openParen).str();
    const std::string terminator = ")" + delimiter + "\"";
    const size_t close = line.find(terminator, openParen + 1);
    if (close == StringRef::npos) {
      i = line.size();
      return true;
    }

    i = close + terminator.size();
    return true;
  }

  return false;
}

bool skipStringOrCharLiteral(StringRef line, size_t &i) {
  const size_t before = i;
  if (skipRawStringLiteral(line, i))
    return true;
  i = before;
  return skipOrdinaryStringOrCharLiteral(line, i);
}

/// Record direct source-spelled location builtins in ordinary final source
/// lines.  Preprocessing directive lines are intentionally excluded: a builtin
/// in a macro replacement list observes line state at a later expansion site,
/// not where the `#define` text appears.  Those expansion-site observers need
/// producer/model evidence and are added by a later chunk.
void collectLexicalObserverCandidates(FinalLineControlModel &model,
                                      StringRef line,
                                      bool startsInBlockComment,
                                      uint64_t finalLineBegin,
                                      uint64_t physicalLineNo,
                                      uint32_t conditionalDepth,
                                      const FinalLogicalState &stateBefore) {
  bool inBlockComment = startsInBlockComment;
  size_t i = 0;
  while (i < line.size()) {
    if (inBlockComment) {
      const size_t close = line.find("*/", i);
      if (close == StringRef::npos)
        return;
      i = close + 2;
      inBlockComment = false;
      continue;
    }

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
      return;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
      const size_t close = line.find("*/", i + 2);
      if (close == StringRef::npos)
        return;
      i = close + 2;
      continue;
    }

    if (skipStringOrCharLiteral(line, i))
      continue;

    if (!isIdentifierStart(line[i])) {
      ++i;
      continue;
    }

    const size_t identBegin = i;
    ++i;
    while (i < line.size() && isIdentifierContinue(line[i]))
      ++i;

    const StringRef ident = line.slice(identBegin, i);
    std::optional<FinalObserver::Kind> kind = observerKindForIdentifier(ident);
    if (!kind)
      continue;

    FinalObserver observer;
    observer.finalOffset = finalLineBegin + identBegin;
    observer.finalEnd = finalLineBegin + i;
    observer.physicalLine = physicalLineNo;
    observer.kind = *kind;
    observer.activity = conditionalDepth == 0
                            ? FinalObserverActivity::KnownActive
                            : FinalObserverActivity::Unknown;
    observer.semanticSource = FinalObserverSemanticSource::LexicalFinalSource;
    observer.producerProven = false;
    observer.spellingPreserved = true;
    observer.stateBefore = stateBefore;
    observer.rawText = ident.str();
    model.AddObserver(std::move(observer));
  }
}


struct DirectiveLogicalStateRecord {
  bool present = false;
  FinalLogicalState stateBefore;
};

enum class ComponentReachKind : uint8_t {
  Reaches,
  DominatedByKnownDirective,
  BlockedByUnknownDirective,
};

struct ComponentReachResult {
  ComponentReachKind kind = ComponentReachKind::Reaches;
  std::optional<size_t> directiveIndex = std::nullopt;
};

FinalLineObserverComponent componentForObserverKind(FinalObserver::Kind kind) {
  switch (kind) {
  case FinalObserver::Kind::Line:
    return FinalLineObserverComponent::Line;
  case FinalObserver::Kind::File:
    return FinalLineObserverComponent::File;
  case FinalObserver::Kind::FileName:
    return FinalLineObserverComponent::FileName;
  }
  llvm_unreachable("invalid final observer kind");
}

StringRef finalBasename(StringRef file) {
  const size_t slash = file.find_last_of("/\\");
  if (slash == StringRef::npos)
    return file;
  return file.drop_front(slash + 1);
}

bool directiveMayOverwriteComponent(const FinalLineDirective &directive,
                                    FinalLineObserverComponent component) {
  if (directive.activity == FinalLineDirectiveActivity::KnownInactive)
    return false;

  if (!directive.semanticsKnown)
    return true;

  switch (component) {
  case FinalLineObserverComponent::Line:
    return directive.line.has_value();
  case FinalLineObserverComponent::File:
  case FinalLineObserverComponent::FileName:
    return directive.file.has_value();
  }
  llvm_unreachable("invalid final observer component");
}

bool directiveKnownActiveWithKnownSemantics(
    const FinalLineDirective &directive) {
  return directive.activity == FinalLineDirectiveActivity::KnownActive &&
         directive.semanticsKnown;
}

bool directiveAffectsComponentRelativeToRemoval(
    const FinalLineDirective &directive, const FinalLogicalState &stateBefore,
    FinalLineObserverComponent component) {
  switch (component) {
  case FinalLineObserverComponent::Line: {
    if (!directive.line)
      return false;
    if (!stateBefore.lineKnown)
      return true;

    // Final pruning removes the directive line itself, not merely the `#line`
    // operator while leaving an otherwise ordinary physical line behind.  The
    // first following line therefore inherits the state that existed before the
    // directive.  A `#line N` is observer-live only if it differs from that
    // removal state and reaches a preserved `__LINE__` observer.
    return *directive.line != stateBefore.logicalLine;
  }
  case FinalLineObserverComponent::File: {
    if (!directive.file)
      return false;
    if (!stateBefore.fileKnown)
      return true;
    return *directive.file != stateBefore.logicalFile;
  }
  case FinalLineObserverComponent::FileName: {
    if (!directive.file)
      return false;
    if (!stateBefore.fileKnown)
      return true;
    return finalBasename(*directive.file) != finalBasename(stateBefore.logicalFile);
  }
  }
  llvm_unreachable("invalid final observer component");
}

void markLive(FinalLineDirectiveObserverLiveness &live,
              FinalLineObserverComponent component) {
  switch (component) {
  case FinalLineObserverComponent::Line:
    live.lineLive = true;
    break;
  case FinalLineObserverComponent::File:
    live.fileLive = true;
    break;
  case FinalLineObserverComponent::FileName:
    live.fileNameLive = true;
    break;
  }
}

void markUnknownDependence(FinalLineDirectiveObserverLiveness &live,
                           FinalLineObserverComponent component) {
  switch (component) {
  case FinalLineObserverComponent::Line:
    live.lineUnknownDependence = true;
    break;
  case FinalLineObserverComponent::File:
    live.fileUnknownDependence = true;
    break;
  case FinalLineObserverComponent::FileName:
    live.fileNameUnknownDependence = true;
    break;
  }
}

ComponentReachResult componentReachFromDirectiveToObserver(
    ArrayRef<FinalLineDirective> directives, size_t directiveIndex,
    const FinalObserver &observer, FinalLineObserverComponent component) {
  for (size_t j = directiveIndex + 1; j < directives.size(); ++j) {
    const FinalLineDirective &later = directives[j];
    if (later.finalBegin >= observer.finalOffset)
      break;

    if (!directiveMayOverwriteComponent(later, component))
      continue;

    if (directiveKnownActiveWithKnownSemantics(later))
      return {ComponentReachKind::DominatedByKnownDirective, j};

    return {ComponentReachKind::BlockedByUnknownDirective, j};
  }

  return {ComponentReachKind::Reaches, std::nullopt};
}

std::vector<DirectiveLogicalStateRecord>
buildDirectiveLogicalStateRecords(const FinalLineControlModel &model) {
  std::vector<DirectiveLogicalStateRecord> records(model.Directives().size());
  for (const FinalPhysicalLine &line : model.PhysicalLines()) {
    if (!line.directiveIndex)
      continue;
    const size_t idx = *line.directiveIndex;
    if (idx >= records.size())
      continue;
    records[idx].present = true;
    records[idx].stateBefore = line.stateBefore;
  }
  return records;
}


struct FinalLayoutSegment {
  size_t beginLine = 0;
  size_t endLine = 0;
  std::optional<size_t> leftVisibleLine = std::nullopt;
  std::optional<size_t> rightVisibleLine = std::nullopt;
  std::vector<size_t> directiveIndices;
  bool hasNonLineLayoutMaterial = false;
  bool hasUnknownLayoutMaterial = false;
  bool hasBlankLine = false;
  bool hasCommentOnlyLine = false;
  bool hasDirectiveOnlyLine = false;
  bool hasUnknownLine = false;
  std::optional<size_t> firstNonLineMaterialLine = std::nullopt;
  std::optional<size_t> lastNonLineMaterialLine = std::nullopt;

  bool IsPrefixBeforeVisible() const {
    return !leftVisibleLine && rightVisibleLine.has_value();
  }

  bool IsGapBetweenVisibleLines() const {
    return leftVisibleLine.has_value() && rightVisibleLine.has_value();
  }

  bool HasRightVisibleTokenLine() const { return rightVisibleLine.has_value(); }

  bool HasBlankLineSensitiveMaterial() const {
    return hasBlankLine || hasCommentOnlyLine || hasDirectiveOnlyLine ||
           hasUnknownLine || hasUnknownLayoutMaterial;
  }

  bool HasOnlyKnownZeroTokenLayoutMaterial() const {
    return hasNonLineLayoutMaterial && !hasUnknownLayoutMaterial;
  }
};

bool finalLineProducesVisibleTokens(const FinalPhysicalLine &line) {
  return line.kind == FinalPhysicalLineKind::Ordinary;
}

bool finalLineIsLineControl(const FinalPhysicalLine &line) {
  return line.kind == FinalPhysicalLineKind::LineDirective &&
         line.directiveIndex.has_value();
}

enum class FinalLayoutMaterialClass : uint8_t {
  None,
  KnownZeroToken,
  UnknownEffect,
};

/// Return true for preprocessing directives whose directive line is known not
/// to contribute text to `clang -E -P` output.
///
/// This is intentionally small.  Directives such as `#include` and `#pragma`
/// can contribute output or depend on external state, and conditional
/// directives change which physical lines are active.  Those remain
/// UnknownEffect so layout pruning keeps surrounding #line directives
/// fail-closed rather than manufacturing a local layout proof.
bool directiveNameIsKnownZeroTokenForLayout(StringRef name) {
  return name == "define" || name == "undef";
}

FinalLayoutMaterialClass classifyFinalLineLayoutMaterial(
    const FinalPhysicalLine &line) {
  switch (line.kind) {
  case FinalPhysicalLineKind::Blank:
  case FinalPhysicalLineKind::CommentOnly:
    return FinalLayoutMaterialClass::KnownZeroToken;
  case FinalPhysicalLineKind::OtherDirective:
    return directiveNameIsKnownZeroTokenForLayout(line.directiveName)
               ? FinalLayoutMaterialClass::KnownZeroToken
               : FinalLayoutMaterialClass::UnknownEffect;
  case FinalPhysicalLineKind::ConditionalDirective:
  case FinalPhysicalLineKind::Unknown:
    return FinalLayoutMaterialClass::UnknownEffect;
  case FinalPhysicalLineKind::Ordinary:
  case FinalPhysicalLineKind::LineDirective:
    return FinalLayoutMaterialClass::None;
  }
  llvm_unreachable("invalid final physical line kind");
}

void noteNonLineLayoutMaterial(FinalLayoutSegment &segment,
                               const FinalPhysicalLine &line,
                               size_t physicalLineIndex) {
  const FinalLayoutMaterialClass materialClass =
      classifyFinalLineLayoutMaterial(line);
  if (materialClass == FinalLayoutMaterialClass::None)
    return;

  segment.hasNonLineLayoutMaterial = true;
  if (materialClass == FinalLayoutMaterialClass::UnknownEffect)
    segment.hasUnknownLayoutMaterial = true;

  if (!segment.firstNonLineMaterialLine)
    segment.firstNonLineMaterialLine = physicalLineIndex;
  segment.lastNonLineMaterialLine = physicalLineIndex;
  switch (line.kind) {
  case FinalPhysicalLineKind::Blank:
    segment.hasBlankLine = true;
    break;
  case FinalPhysicalLineKind::CommentOnly:
    segment.hasCommentOnlyLine = true;
    break;
  case FinalPhysicalLineKind::OtherDirective:
  case FinalPhysicalLineKind::ConditionalDirective:
    segment.hasDirectiveOnlyLine = true;
    break;
  case FinalPhysicalLineKind::Unknown:
    segment.hasUnknownLine = true;
    break;
  case FinalPhysicalLineKind::Ordinary:
  case FinalPhysicalLineKind::LineDirective:
    break;
  }
}

bool directiveKnownActiveWithKnownLayoutSemantics(
    const FinalLineDirective &directive) {
  return directive.activity == FinalLineDirectiveActivity::KnownActive &&
         directive.semanticsKnown;
}

std::vector<FinalLayoutSegment>
analyzeFinalLayoutSegments(const FinalLineControlModel &model) {
  std::vector<FinalLayoutSegment> segments;
  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();

  std::optional<size_t> previousVisibleLine = std::nullopt;
  size_t i = 0;
  while (i < lines.size()) {
    if (finalLineProducesVisibleTokens(lines[i])) {
      previousVisibleLine = i;
      ++i;
      continue;
    }

    FinalLayoutSegment segment;
    segment.beginLine = i;
    segment.leftVisibleLine = previousVisibleLine;

    while (i < lines.size() && !finalLineProducesVisibleTokens(lines[i])) {
      const FinalPhysicalLine &line = lines[i];
      if (finalLineIsLineControl(line))
        segment.directiveIndices.push_back(*line.directiveIndex);
      noteNonLineLayoutMaterial(segment, line, i);
      ++i;
    }

    segment.endLine = i;
    if (i < lines.size() && finalLineProducesVisibleTokens(lines[i]))
      segment.rightVisibleLine = i;

    if (!segment.directiveIndices.empty())
      segments.push_back(std::move(segment));
  }

  return segments;
}

std::optional<size_t> directivePhysicalLineInSegment(
    const FinalLayoutSegment &segment, ArrayRef<FinalPhysicalLine> lines,
    size_t directiveIndex) {
  for (size_t lineIndex = segment.beginLine; lineIndex < segment.endLine;
       ++lineIndex) {
    if (lineIndex >= lines.size())
      break;
    if (lines[lineIndex].directiveIndex &&
        *lines[lineIndex].directiveIndex == directiveIndex)
      return lineIndex;
  }
  return std::nullopt;
}

bool directiveCanSeparateLayoutMaterialFromVisibleLine(
    const FinalLayoutSegment &segment, ArrayRef<FinalPhysicalLine> lines,
    size_t directiveIndex) {
  if (!segment.HasRightVisibleTokenLine() || !segment.hasNonLineLayoutMaterial ||
      !segment.lastNonLineMaterialLine)
    return false;

  std::optional<size_t> directiveLine =
      directivePhysicalLineInSegment(segment, lines, directiveIndex);
  if (!directiveLine)
    return false;

  // A final-stream layout barrier must occur after the zero-token material it
  // is claimed to normalize and before the visible line whose -E -P placement
  // is being protected.  A #line that appears before a comment/directive-only
  // gap is not itself the final barrier for that gap; either a later directive
  // discharges the obligation or the gap remains fail-closed.
  return *segment.lastNonLineMaterialLine < *directiveLine &&
         (!segment.rightVisibleLine || *directiveLine < *segment.rightVisibleLine);
}

std::optional<size_t> canonicalKnownLayoutDirectiveForSegment(
    const FinalLayoutSegment &segment, ArrayRef<FinalPhysicalLine> lines,
    ArrayRef<FinalLineDirective> directives) {
  if (!segment.HasRightVisibleTokenLine() || !segment.hasNonLineLayoutMaterial ||
      !segment.lastNonLineMaterialLine)
    return std::nullopt;

  for (size_t reverse = segment.directiveIndices.size(); reverse > 0; --reverse) {
    const size_t directiveIndex = segment.directiveIndices[reverse - 1];
    if (directiveIndex >= directives.size())
      continue;
    if (!directiveCanSeparateLayoutMaterialFromVisibleLine(segment, lines,
                                                          directiveIndex))
      continue;
    if (directiveKnownActiveWithKnownLayoutSemantics(directives[directiveIndex]))
      return directiveIndex;
  }

  return std::nullopt;
}

void markLayoutLive(FinalLineDirectiveLayoutLiveness &live,
                    FinalLayoutObligation::Kind kind) {
  switch (kind) {
  case FinalLayoutObligation::Kind::ZeroTokenPrefixBarrier:
    live.zeroTokenPrefixBarrierLive = true;
    break;
  case FinalLayoutObligation::Kind::ZeroTokenGapBarrier:
    live.zeroTokenGapBarrierLive = true;
    break;
  case FinalLayoutObligation::Kind::FirstVisibleTokenAlignment:
    live.firstVisibleTokenAlignmentLive = true;
    break;
  case FinalLayoutObligation::Kind::BlankLinePreservation:
    live.blankLinePreservationLive = true;
    break;
  }
}


/// Return true iff \p line is locally known to produce no preprocessing tokens
/// in the final `clang -E -P` stream.  This helper is intentionally narrower
/// than the general layout classifier: it is used only to prove that a
/// synthetic resync directive appears immediately before a simple include and
/// that a later line directive repairs the parent stream before any source line
/// could observe the resync.
bool isLocallyZeroTokenFinalLineForPreIncludeResync(
    const FinalPhysicalLine &line) {
  if (line.kind == FinalPhysicalLineKind::Blank ||
      line.kind == FinalPhysicalLineKind::CommentOnly)
    return true;
  if (line.kind == FinalPhysicalLineKind::ConditionalDirective)
    return true;
  if (line.kind != FinalPhysicalLineKind::OtherDirective)
    return false;

  StringRef name(line.directiveName);
  return name == "define" || name == "undef";
}

/// Return true iff \p line is a simple literal include directive.  Macro-based
/// include operands are deliberately rejected: macro expansion in a directive
/// operand could itself observe the current logical line/file state, so a
/// pre-include resync may not be elided by this proof.
bool isSimpleLiteralIncludeDirectiveLine(const FinalPhysicalLine &line) {
  if (line.kind != FinalPhysicalLineKind::OtherDirective ||
      line.directiveName != "include")
    return false;

  StringRef text(line.rawText);
  text = text.ltrim(" \t\v\f\r");
  if (!text.consume_front("#"))
    return false;
  text = text.ltrim(" \t\v\f\r");
  if (!text.consume_front("include"))
    return false;
  if (!text.empty()) {
    const char next = text.front();
    if (std::isalnum(static_cast<unsigned char>(next)) || next == '_')
      return false;
  }
  text = text.ltrim(" \t\v\f\r");
  return text.starts_with("\"") || text.starts_with("<");
}

std::optional<size_t> findPhysicalLineForDirective(
    const FinalLineControlModel &model, size_t directiveIndex) {
  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].directiveIndex && *lines[i].directiveIndex == directiveIndex)
      return i;
  }
  return std::nullopt;
}

/// A local synthetic newline-resync immediately before a preserved `#include`
/// is often stale: the include pushes its child logical file/line state before
/// any child token can observe the parent resync, and a later parent-side
/// `#line` may repair the stream before the parent suffix observes it.
///
/// This recognizes only that proof shape:
///   synthetic #line
///   #include "literal.h"       or       #include <literal.h>
///   zero-token preprocessor material, or material guarded by the same
///     conditional island as the include arm
///   parent-side #line repair
///
/// The guarded-material allowance is what makes nested conditional/include
/// refolding work without retaining stale local resyncs.  A selected arm can
/// contain a preserved include and then rejoin through inactive sibling arms;
/// those sibling arms may contain ordinary source text in the final file even
/// though the candidate resync cannot reach them on the validated PP path.  We
/// therefore allow non-zero-token-looking physical lines only when the resync
/// itself is inside conditional structure and the intervening line is still
/// inside conditional structure.  The proof remains fail-closed for top-level
/// active source text and is still discharged only after executable validation
/// accepts the physical deletion.
///
/// Any preserved final observer before the repair keeps the directive
/// fail-closed, because such an observer could read the stale parent state.
bool syntheticNewlineResyncIsStaleBeforeSimpleInclude(
    const FinalLineControlModel &model, size_t directiveIndex) {
  const std::optional<size_t> directiveLineIndex =
      findPhysicalLineForDirective(model, directiveIndex);
  if (!directiveLineIndex)
    return false;

  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();
  const FinalPhysicalLine &directiveLine = lines[*directiveLineIndex];
  const bool directiveInsideConditional =
      directiveLine.conditionalDepthBefore != 0 ||
      directiveLine.conditionalDepthAfter != 0;

  size_t i = *directiveLineIndex + 1;
  while (i < lines.size() &&
         (lines[i].kind == FinalPhysicalLineKind::Blank ||
          lines[i].kind == FinalPhysicalLineKind::CommentOnly))
    ++i;
  if (i >= lines.size() || !isSimpleLiteralIncludeDirectiveLine(lines[i]))
    return false;

  const uint64_t guardedBegin = directiveLine.finalEnd;
  std::optional<uint64_t> repairBegin;
  for (++i; i < lines.size(); ++i) {
    const FinalPhysicalLine &line = lines[i];
    if (line.kind == FinalPhysicalLineKind::LineDirective) {
      repairBegin = line.finalBegin;
      break;
    }
    if (isLocallyZeroTokenFinalLineForPreIncludeResync(line))
      continue;

    const bool lineStillInsideConditional =
        line.conditionalDepthBefore != 0 || line.conditionalDepthAfter != 0;
    if (directiveInsideConditional && lineStillInsideConditional)
      continue;

    return false;
  }
  if (!repairBegin)
    return false;

  for (const FinalObserver &observer : model.Observers()) {
    if (!observer.spellingPreserved)
      continue;
    if (observer.activity == FinalObserverActivity::KnownInactive)
      continue;
    if (observer.finalOffset >= guardedBegin &&
        observer.finalOffset < *repairBegin)
      return false;
  }

  return true;
}


/// Return true iff every preserved line/file observer after a synthetic include
/// entry is dominated by a later known-active line-control directive before the
/// observer can read the include-entry state.
///
/// Unknown intervening line-control is not itself a proof of deadness, but it is
/// also not a proof that the include-entry state reaches the observer: a later
/// known-active directive for the observed component makes all earlier line
/// state irrelevant.  This captures header fragments such as:
///
///   #line 1 "header.h"          // synthetic include entry
///   ... zero/token material ...
///   #line MACRO_LINE MACRO_FILE  // semantics unknown to the final scanner
///   ... inserted material ...
///   #line 510 "logical.h"       // known repair
///   int value = __LINE__;
///
/// The executable final-prune validator still has to accept the physical
/// deletion.  This helper only proves that preserved observers do not require
/// the include-entry state.
bool syntheticIncludeEntryStateIsDominatedBeforePreservedObservers(
    const FinalLineControlModel &model, size_t directiveIndex) {
  const ArrayRef<FinalLineDirective> directives = model.Directives();
  if (directiveIndex >= directives.size())
    return false;

  const FinalLineDirective &candidate = directives[directiveIndex];
  for (const FinalObserver &observer : model.Observers()) {
    if (!observer.spellingPreserved)
      continue;
    if (observer.activity == FinalObserverActivity::KnownInactive)
      continue;
    if (observer.finalOffset < candidate.finalEnd)
      continue;

    const FinalLineObserverComponent component =
        componentForObserverKind(observer.kind);
    bool dominated = false;
    for (size_t j = directiveIndex + 1; j < directives.size(); ++j) {
      const FinalLineDirective &later = directives[j];
      if (later.finalBegin >= observer.finalOffset)
        break;

      if (!directiveMayOverwriteComponent(later, component))
        continue;

      if (directiveKnownActiveWithKnownSemantics(later)) {
        dominated = true;
        break;
      }

      // Keep scanning.  Unknown line-control can make the candidate state even
      // less likely to reach the observer, but only a later known-active repair
      // is allowed to discharge this proof.
    }

    if (!dominated)
      return false;
  }

  return true;
}

void collectFinalLayoutObligations(FinalLineControlModel &model) {
  const ArrayRef<FinalLineDirective> directives = model.Directives();
  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();
  const std::vector<FinalLayoutSegment> segments =
      analyzeFinalLayoutSegments(model);

  for (const FinalLayoutSegment &segment : segments) {
    if (!segment.HasRightVisibleTokenLine() || !segment.hasNonLineLayoutMaterial)
      continue;

    // Layout obligations are proof objects, so create them only when every
    // non-line material line in the segment is locally known to be zero-token
    // under `clang -E -P`.  Segments containing includes, pragmas, conditionals,
    // unknown directives, or unknown physical lines are handled fail-closed in
    // ComputeFinalLayoutLiveness instead of manufacturing a possibly false
    // layout proof.
    if (!segment.HasOnlyKnownZeroTokenLayoutMaterial())
      continue;

    const std::optional<size_t> directiveIndex =
        canonicalKnownLayoutDirectiveForSegment(segment, lines, directives);
    if (!directiveIndex)
      continue;

    const uint64_t obligationOffset = lines[*segment.rightVisibleLine].finalBegin;
    auto addObligation = [&](FinalLayoutObligation::Kind kind,
                             StringRef reason) {
      FinalLayoutObligation obligation;
      obligation.finalOffset = obligationOffset;
      obligation.kind = kind;
      obligation.directiveIndex = directiveIndex;
      obligation.reason = reason.str();
      model.AddLayoutObligation(std::move(obligation));
    };

    if (segment.IsPrefixBeforeVisible()) {
      addObligation(FinalLayoutObligation::Kind::ZeroTokenPrefixBarrier,
                    "line-control separates zero-token prefix material from the first visible token line");
      addObligation(FinalLayoutObligation::Kind::FirstVisibleTokenAlignment,
                    "line-control is the closest known barrier before the first visible token line");
    } else if (segment.IsGapBetweenVisibleLines()) {
      addObligation(FinalLayoutObligation::Kind::ZeroTokenGapBarrier,
                    "line-control separates a zero-token gap between visible token lines");
    }

    if (segment.HasBlankLineSensitiveMaterial()) {
      addObligation(FinalLayoutObligation::Kind::BlankLinePreservation,
                    "line-control participates in -E -P blank-line preservation around zero-token material");
    }
  }
}

} // namespace

std::vector<FinalLineDirectiveObserverLiveness>
ComputeFinalObserverLiveness(const FinalLineControlModel &model) {
  std::vector<FinalLineDirectiveObserverLiveness> out;
  out.reserve(model.Directives().size());

  const ArrayRef<FinalLineDirective> directives = model.Directives();
  const ArrayRef<FinalObserver> observers = model.Observers();
  const std::vector<DirectiveLogicalStateRecord> directiveStates =
      buildDirectiveLogicalStateRecords(model);

  for (size_t i = 0; i < directives.size(); ++i) {
    const FinalLineDirective &directive = directives[i];

    FinalLineDirectiveObserverLiveness live;
    live.directiveIndex = i;
    live.finalBegin = directive.finalBegin;
    live.finalEnd = directive.finalEnd;

    auto addReason = [&](StringRef reason) {
      live.reasons.push_back(reason.str());
    };

    if (directive.activity == FinalLineDirectiveActivity::KnownInactive) {
      addReason("directive is known inactive as line-control; physical-line "
                "removal is outside observer pruning");
      markUnknownDependence(live, FinalLineObserverComponent::Line);
      out.push_back(std::move(live));
      continue;
    }

    if (directive.activity != FinalLineDirectiveActivity::KnownActive) {
      addReason("directive activity is not proven; observer pruning is fail-closed");
      markUnknownDependence(live, FinalLineObserverComponent::Line);
      markUnknownDependence(live, FinalLineObserverComponent::File);
      markUnknownDependence(live, FinalLineObserverComponent::FileName);
      out.push_back(std::move(live));
      continue;
    }

    if (!directive.semanticsKnown) {
      addReason("directive semantics are unknown; observer pruning is fail-closed");
      markUnknownDependence(live, FinalLineObserverComponent::Line);
      markUnknownDependence(live, FinalLineObserverComponent::File);
      markUnknownDependence(live, FinalLineObserverComponent::FileName);
      out.push_back(std::move(live));
      continue;
    }

    if (i >= directiveStates.size() || !directiveStates[i].present) {
      addReason("directive has no scanner state record; observer pruning is fail-closed");
      markUnknownDependence(live, FinalLineObserverComponent::Line);
      markUnknownDependence(live, FinalLineObserverComponent::File);
      markUnknownDependence(live, FinalLineObserverComponent::FileName);
      out.push_back(std::move(live));
      continue;
    }

    const FinalLogicalState &stateBefore = directiveStates[i].stateBefore;
    const bool affectsLine = directiveAffectsComponentRelativeToRemoval(
        directive, stateBefore, FinalLineObserverComponent::Line);
    const bool affectsFile = directiveAffectsComponentRelativeToRemoval(
        directive, stateBefore, FinalLineObserverComponent::File);
    const bool affectsFileName = directiveAffectsComponentRelativeToRemoval(
        directive, stateBefore, FinalLineObserverComponent::FileName);

    if (!affectsLine && !affectsFile && !affectsFileName) {
      addReason("directive establishes the same line/file/filename state as directive removal");
      live.observerDead = true;
      live.removableIfLayoutDead = true;
      out.push_back(std::move(live));
      continue;
    }

    auto componentAffected = [&](FinalLineObserverComponent component) -> bool {
      switch (component) {
      case FinalLineObserverComponent::Line:
        return affectsLine;
      case FinalLineObserverComponent::File:
        return affectsFile;
      case FinalLineObserverComponent::FileName:
        return affectsFileName;
      }
      llvm_unreachable("invalid final observer component");
    };

    for (const FinalObserver &observer : observers) {
      if (!observer.spellingPreserved)
        continue;
      if (observer.activity == FinalObserverActivity::KnownInactive)
        continue;
      if (observer.finalOffset < directive.finalEnd)
        continue;

      const FinalLineObserverComponent component =
          componentForObserverKind(observer.kind);
      if (!componentAffected(component))
        continue;

      const ComponentReachResult reach =
          componentReachFromDirectiveToObserver(directives, i, observer,
                                                component);
      switch (reach.kind) {
      case ComponentReachKind::Reaches:
        if (observer.activity == FinalObserverActivity::KnownActive) {
          markLive(live, component);
          live.reasons.push_back(
              formatv("{0} reaches preserved {1} observer at [{2},{3})",
                      component, observer.kind, observer.finalOffset,
                      observer.finalEnd)
                  .str());
        } else {
          markUnknownDependence(live, component);
          live.reasons.push_back(
              formatv("{0} reaches observer with unknown activity at [{1},{2}); fail-closed",
                      component, observer.finalOffset, observer.finalEnd)
                  .str());
        }
        break;
      case ComponentReachKind::DominatedByKnownDirective:
        live.reasons.push_back(
            formatv("{0} is dominated before observer [{1},{2}) by directive#{3}",
                    component, observer.finalOffset, observer.finalEnd,
                    reach.directiveIndex)
                .str());
        break;
      case ComponentReachKind::BlockedByUnknownDirective:
        markUnknownDependence(live, component);
        live.reasons.push_back(
            formatv("{0} reach to observer [{1},{2}) crosses unknown directive#{3}; fail-closed",
                    component, observer.finalOffset, observer.finalEnd,
                    reach.directiveIndex)
                .str());
        break;
      }
    }

    live.observerDead = !live.HasObserverLiveComponent() &&
                        !live.HasUnknownDependence();
    live.removableIfLayoutDead = live.observerDead;
    if (live.observerDead)
      addReason("no preserved observer consumes this directive after component dominance");

    out.push_back(std::move(live));
  }

  return out;
}

std::vector<FinalLineDirectiveLayoutLiveness>
ComputeFinalLayoutLiveness(const FinalLineControlModel &model) {
  std::vector<FinalLineDirectiveLayoutLiveness> out;
  out.reserve(model.Directives().size());

  const ArrayRef<FinalLineDirective> directives = model.Directives();
  for (size_t i = 0; i < directives.size(); ++i) {
    FinalLineDirectiveLayoutLiveness live;
    live.directiveIndex = i;
    live.finalBegin = directives[i].finalBegin;
    live.finalEnd = directives[i].finalEnd;
    out.push_back(std::move(live));
  }

  std::vector<bool> hasPhysicalLineRecord(directives.size(), false);
  for (const FinalPhysicalLine &line : model.PhysicalLines()) {
    if (line.directiveIndex && *line.directiveIndex < hasPhysicalLineRecord.size())
      hasPhysicalLineRecord[*line.directiveIndex] = true;
  }

  for (size_t i = 0; i < directives.size(); ++i) {
    if (!hasPhysicalLineRecord[i]) {
      out[i].layoutUnknownDependence = true;
      out[i].reasons.push_back(
          "directive has no final physical-line record; layout pruning is fail-closed");
    }
  }

  const std::vector<FinalLayoutSegment> segments =
      analyzeFinalLayoutSegments(model);
  for (const FinalLayoutSegment &segment : segments) {
    const bool layoutSensitive = segment.HasRightVisibleTokenLine() &&
                                 segment.hasNonLineLayoutMaterial;
    const std::optional<size_t> canonicalDirective =
        canonicalKnownLayoutDirectiveForSegment(segment, model.PhysicalLines(),
                                               directives);

    for (size_t directiveIndex : segment.directiveIndices) {
      if (directiveIndex >= out.size())
        continue;

      FinalLineDirectiveLayoutLiveness &live = out[directiveIndex];
      const FinalLineDirective &directive = directives[directiveIndex];

      if (!layoutSensitive) {
        if (!segment.HasRightVisibleTokenLine() &&
            segment.hasUnknownLayoutMaterial) {
          live.layoutUnknownDependence = true;
          live.reasons.push_back(
              "line-control is in a trailing segment with directive/unknown "
              "material whose -E -P effect is not locally modeled; fail-closed");
        } else if (!segment.HasRightVisibleTokenLine()) {
          live.reasons.push_back(
              "line-control is in a trailing known-zero-token segment with no "
              "following visible token line");
        } else {
          live.reasons.push_back(
              "line-control segment has no zero-token layout material besides line-control itself");
        }
        continue;
      }

      const bool canSeparateLayoutMaterial =
          directiveCanSeparateLayoutMaterialFromVisibleLine(
              segment, model.PhysicalLines(), directiveIndex);

      if (segment.hasUnknownLayoutMaterial) {
        live.layoutUnknownDependence = true;
        live.reasons.push_back(
            "layout-sensitive segment contains directive/unknown material whose "
            "-E -P effect is not locally modeled; fail-closed");
        continue;
      }

      if (!canSeparateLayoutMaterial) {
        live.reasons.push_back(
            "line-control occurs before the last known zero-token layout line; "
            "it cannot be the final barrier before the next visible token line");
        continue;
      }

      if (!directiveKnownActiveWithKnownLayoutSemantics(directive)) {
        live.layoutUnknownDependence = true;
        live.reasons.push_back(
            "line-control is positioned as a possible layout barrier but "
            "activity/semantics are not fully proven; fail-closed");
        continue;
      }

      if (canonicalDirective && *canonicalDirective == directiveIndex) {
        live.reasons.push_back(
            "line-control is the closest known active barrier after the last "
            "known zero-token layout line and before the next visible token line");
        continue;
      }

      if (canonicalDirective) {
        live.reasons.push_back(
            formatv("layout obligation is discharged by closer directive#{0}",
                    *canonicalDirective)
                .str());
      } else {
        // With only known zero-token material, the only layout-live directive in
        // the segment would be a known active directive after the last material
        // line.  Since this directive has that shape and no canonical directive
        // exists, keep the case fail-closed rather than silently claiming a
        // malformed scanner state is layout-dead.
        live.layoutUnknownDependence = true;
        live.reasons.push_back(
            "layout-sensitive known-zero segment has a possible barrier but no "
            "canonical known active directive; fail-closed");
      }
    }
  }

  for (const FinalLayoutObligation &obligation : model.LayoutObligations()) {
    if (!obligation.directiveIndex || *obligation.directiveIndex >= out.size())
      continue;

    FinalLineDirectiveLayoutLiveness &live = out[*obligation.directiveIndex];
    markLayoutLive(live, obligation.kind);
    live.reasons.push_back(
        formatv("discharges {0} obligation at final offset {1}: {2}",
                obligation.kind, obligation.finalOffset, obligation.reason)
            .str());
  }

  for (FinalLineDirectiveLayoutLiveness &live : out) {
    live.layoutDead = !live.HasLayoutLiveComponent() &&
                      !live.layoutUnknownDependence;
    live.removableIfObserverDead = live.layoutDead;
    if (live.layoutDead)
      live.reasons.push_back(
          "no final-stream layout obligation consumes this directive");
  }

  return out;
}

FinalLineControlModel CollectFinalLineControlModel(
    StringRef finalSource,
    ArrayRef<FinalLineControlPruneCandidate> removableCandidates,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings,
    ArrayRef<FinalLineControlProducerEvent> producerEvents,
    ArrayRef<FinalLineControlProducerObserver> producerObservers) {
  std::vector<FinalLineControlPruneCandidate> canonicalCandidates(
      removableCandidates.begin(), removableCandidates.end());
  CanonicalizeFinalLineControlPruneCandidates(canonicalCandidates);

  std::vector<FinalLineControlSourceMapping> canonicalSourceMappings(
      sourceMappings.begin(), sourceMappings.end());
  CanonicalizeFinalLineControlSourceMappings(canonicalSourceMappings);

  FinalLineControlModel model;
  for (const FinalLineControlSourceMapping &mapping : canonicalSourceMappings)
    model.AddSourceMapping(mapping);
  FinalLogicalState state = FinalLogicalState::Initial();

  bool inBlockComment = false;
  bool inDirectiveContinuation = false;
  std::string continuedDirectiveName;
  uint32_t conditionalDepth = 0;
  bool unresolvedConditionalLineControl = false;
  bool unresolvedConditionalFileControl = false;

  size_t lineBegin = 0;
  uint64_t physicalLineNo = 1;
  while (lineBegin < finalSource.size()) {
    size_t lineEndNoNl = finalSource.find('\n', lineBegin);
    const bool hasNewline = lineEndNoNl != StringRef::npos;
    if (!hasNewline)
      lineEndNoNl = finalSource.size();

    const size_t lineEnd = hasNewline ? lineEndNoNl + 1 : lineEndNoNl;
    const StringRef physicalLine = finalSource.slice(lineBegin, lineEndNoNl);

    FinalPhysicalLine lineRecord;
    lineRecord.finalBegin = lineBegin;
    lineRecord.finalEnd = lineEnd;
    lineRecord.physicalLine = physicalLineNo;
    lineRecord.conditionalDepthBefore = conditionalDepth;
    lineRecord.stateBefore = state;
    lineRecord.rawText = finalSource.slice(lineBegin, lineEnd).str();

    size_t hash = 0;
    const bool lineStartsInBlockComment = inBlockComment;
    const bool lineIsDirectiveContinuation = inDirectiveContinuation;
    bool hasDirective = false;
    DirectiveInfo directive;
    ParsedLineControl parsedLineControl;
    if (lineIsDirectiveContinuation) {
      // This physical line is part of a preceding preprocessing directive after
      // phase-2 line splicing.  It inherits that directive's identifier for
      // layout classification, so a multi-line #define remains known zero-token
      // material while continuations of #include/#pragma remain fail-closed.
      lineRecord.kind = FinalPhysicalLineKind::OtherDirective;
      lineRecord.directiveName = continuedDirectiveName;
    } else {
      hasDirective = locateDirectiveHash(physicalLine, inBlockComment, hash);
      if (hasDirective) {
        directive = classifyDirective(physicalLine, hash);
        lineRecord.kind = physicalKindForDirective(directive.kind);
        lineRecord.directiveName = directive.name.str();
        if (directive.kind == DirectiveKind::LineControl)
          parsedLineControl = parseLineControl(physicalLine, directive);
      } else {
        lineRecord.kind =
            classifyNonDirectiveLine(physicalLine, lineStartsInBlockComment);
      }
    }

    if (lineRecord.kind == FinalPhysicalLineKind::Ordinary) {
      collectLexicalObserverCandidates(model, physicalLine,
                                       lineStartsInBlockComment, lineBegin,
                                       physicalLineNo, conditionalDepth,
                                       lineRecord.stateBefore);
    }

    collectProducerObserverCandidatesForLine(
        model, finalSource, lineBegin, lineEnd, physicalLineNo,
        lineRecord.stateBefore, canonicalSourceMappings, producerObservers);

    if (directive.kind == DirectiveKind::LineControl) {
      FinalLineDirective lineDirective(
          lineBegin, lineEnd, FinalLineDirective::Origin::Unknown,
          finalSource.slice(lineBegin, lineEnd).str());
      lineDirective.line = parsedLineControl.line;
      lineDirective.file = parsedLineControl.file;
      lineDirective.semanticsKnown = parsedLineControl.semanticsKnown;
      lineDirective.semanticSource = parsedLineControl.semanticsKnown
                                         ? FinalLineDirectiveSemanticSource::LiteralFinalSource
                                         : FinalLineDirectiveSemanticSource::Unknown;
      lineDirective.activity = conditionalDepth == 0
                                   ? FinalLineDirectiveActivity::KnownActive
                                   : FinalLineDirectiveActivity::Unknown;

      applyMatchingPruneCandidate(lineDirective, canonicalCandidates);

      if (std::optional<FinalLineControlSourceMapping> sourceMapping =
              mapFinalDirectiveToSource(lineBegin, lineEnd, canonicalSourceMappings)) {
        if (const FinalLineControlProducerEvent *event =
                findProducerLineControlEvent(*sourceMapping, lineDirective.rawText,
                                             parsedLineControl, producerEvents)) {
          applyProducerLineControlEvent(lineDirective, *sourceMapping, *event);
          markProducerProvenSourceLineControlPreserved(lineDirective,
                                                       *sourceMapping, *event);
        }
      }

      const bool knownActiveProducerDirective =
          lineDirective.activity == FinalLineDirectiveActivity::KnownActive &&
          lineDirective.semanticsKnown && lineDirective.line.has_value();
      const size_t directiveIndex = model.AddDirective(std::move(lineDirective));
      lineRecord.directiveIndex = directiveIndex;

      const FinalLineDirective &recordedDirective =
          model.Directives()[directiveIndex];
      if (conditionalDepth == 0 || knownActiveProducerDirective) {
        if (recordedDirective.semanticsKnown && recordedDirective.line) {
          state.ApplyKnownLineDirective(*recordedDirective.line,
                                        recordedDirective.file);
        } else {
          state.MarkUnknown();
        }
      } else {
        unresolvedConditionalLineControl = true;
        if (directiveMayChangeFile(parsedLineControl))
          unresolvedConditionalFileControl = true;
        state.AdvancePhysicalLine();
      }
    } else {
      state.AdvancePhysicalLine();
    }

    switch (directive.kind) {
    case DirectiveKind::ConditionalEnter:
      ++conditionalDepth;
      break;
    case DirectiveKind::ConditionalMiddle:
      break;
    case DirectiveKind::ConditionalExit:
      if (conditionalDepth > 0)
        --conditionalDepth;
      if (conditionalDepth == 0) {
        if (unresolvedConditionalLineControl)
          state.MarkLineUnknown();
        if (unresolvedConditionalFileControl)
          state.MarkFileUnknown();
        unresolvedConditionalLineControl = false;
        unresolvedConditionalFileControl = false;
      }
      break;
    case DirectiveKind::None:
    case DirectiveKind::Other:
    case DirectiveKind::LineControl:
      break;
    }

    const bool continuesDirective =
        (lineIsDirectiveContinuation || hasDirective) &&
        physicalLineContinuesWithBackslash(physicalLine);
    if (continuesDirective) {
      if (hasDirective)
        continuedDirectiveName = directive.name.str();
    } else {
      continuedDirectiveName.clear();
    }
    inDirectiveContinuation = continuesDirective;

    lineRecord.conditionalDepthAfter = conditionalDepth;
    lineRecord.stateAfter = state;
    model.AddPhysicalLine(std::move(lineRecord));

    if (!hasNewline)
      break;
    lineBegin = lineEnd;
    ++physicalLineNo;
  }

  collectFinalLayoutObligations(model);
  return model;
}

FinalLineControlModel
CollectPassiveFinalLineControlModel(
    StringRef finalSource, ArrayRef<FinalLineControlSourceMapping> sourceMappings,
    ArrayRef<FinalLineControlProducerEvent> producerEvents,
    ArrayRef<FinalLineControlProducerObserver> producerObservers) {
  return CollectFinalLineControlModel(finalSource,
                                      ArrayRef<FinalLineControlPruneCandidate>(),
                                      sourceMappings, producerEvents,
                                      producerObservers);
}



FinalLineControlPruneResult
PruneFinalLineControlDirectives(
    StringRef finalSource,
    ArrayRef<FinalLineControlPruneCandidate> removableCandidates,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings,
    ArrayRef<FinalLineControlProducerEvent> producerEvents,
    ArrayRef<FinalLineControlProducerObserver> producerObservers,
    FinalLineControlValidationCallback validationCallback) {
  FinalLineControlPruneResult result;
  std::string current = finalSource.str();
  std::vector<FinalLineControlPruneCandidate> currentCandidates(
      removableCandidates.begin(), removableCandidates.end());
  CanonicalizeFinalLineControlPruneCandidates(currentCandidates);
  std::vector<FinalLineControlSourceMapping> currentSourceMappings(
      sourceMappings.begin(), sourceMappings.end());
  CanonicalizeFinalLineControlSourceMappings(currentSourceMappings);
  const bool recordDecisions = inTraceMode();

  for (;;) {
    FinalLineControlModel model =
        CollectFinalLineControlModel(current, currentCandidates,
                                     currentSourceMappings, producerEvents,
                                     producerObservers);
    const ArrayRef<FinalLineDirective> directives = model.Directives();
    const std::vector<FinalLineDirectiveObserverLiveness> observerLiveness =
        ComputeFinalObserverLiveness(model);
    const std::vector<FinalLineDirectiveLayoutLiveness> layoutLiveness =
        ComputeFinalLayoutLiveness(model);

    bool removedThisIteration = false;
    const uint32_t iteration = result.iterations;

    for (size_t i = 0; i < directives.size(); ++i) {
      const FinalLineDirective &directive = directives[i];

      FinalLineControlPruneDecision decision;
      decision.iteration = iteration;
      decision.directiveIndex = i;
      decision.finalBegin = directive.finalBegin;
      decision.finalEnd = directive.finalEnd;
      decision.explicitCandidate = directive.removable;

      auto addReason = [&](StringRef reason) {
        if (recordDecisions)
          decision.reasons.push_back(reason.str());
      };

      if (!directive.removable)
        addReason("directive is not an explicit removable candidate; "
                  "fixed-point pruning is fail-closed");

      if (directive.activity != FinalLineDirectiveActivity::KnownActive)
        addReason("directive activity is not known-active; cannot physically "
                  "remove");

      if (!directive.semanticsKnown)
        addReason("directive semantics are unknown; cannot physically remove");

      if (i < observerLiveness.size()) {
        decision.observerDead = observerLiveness[i].observerDead;
        if (!observerLiveness[i].observerDead)
          addReason("observer liveness does not prove the directive dead");
      } else {
        addReason("missing observer-liveness record; cannot physically "
                  "remove");
      }

      if (i < layoutLiveness.size()) {
        decision.layoutDead = layoutLiveness[i].layoutDead;
        if (!layoutLiveness[i].layoutDead)
          addReason("layout liveness does not prove the directive dead");
      } else {
        addReason("missing layout-liveness record; cannot physically remove");
      }

      const bool validRange = directive.finalBegin < directive.finalEnd &&
                              directive.finalEnd <= current.size();
      if (!validRange)
        addReason("directive final byte range is invalid; cannot physically "
                  "remove");

      auto isValidationDischargeableSyntheticIncludeEntry =
          [](FinalLineDirective::Origin origin) {
            switch (origin) {
            case FinalLineDirective::Origin::SyntheticIncludeEntry:
              return true;
            case FinalLineDirective::Origin::PreservedSource:
            case FinalLineDirective::Origin::SyntheticIncludeReturn:
            case FinalLineDirective::Origin::SyntheticNewlineResync:
            case FinalLineDirective::Origin::SyntheticSourceLineResume:
            case FinalLineDirective::Origin::SyntheticTUPrologue:
            case FinalLineDirective::Origin::SyntheticLayoutBarrier:
            case FinalLineDirective::Origin::Unknown:
              return false;
            }
            llvm_unreachable("invalid final line directive origin");
          };

      const bool hasObserverRecord = i < observerLiveness.size();
      const bool hasLayoutRecord = i < layoutLiveness.size();
      const bool hasConcreteObserverLiveComponent =
          hasObserverRecord && observerLiveness[i].HasObserverLiveComponent();
      const bool hasConcreteLayoutLiveComponent =
          hasLayoutRecord && layoutLiveness[i].HasLayoutLiveComponent();
      const bool includeEntryObserversDominated =
          isValidationDischargeableSyntheticIncludeEntry(directive.origin) &&
          syntheticIncludeEntryStateIsDominatedBeforePreservedObservers(model,
                                                                       i);

      // Synthetic include-entry line directives are conservative wrappers for
      // materialized header text.  They can be stale in two general shapes:
      //
      //   * no preserved line/file observer exists in the materialized region;
      //   * every later preserved observer is dominated by a subsequent
      //     known-active line-control repair before the observer is reached.
      //
      // Layout liveness is intentionally not a hard blocker for this narrow
      // synthetic-origin path: the executable final-prune validator must still
      // prove that deleting the directive preserves the preprocessed token
      // stream.  Source-authored directives and return/resync/TU-prologue
      // repairs still require the ordinary model proof before validation is
      // allowed to delete them.
      const bool validationMayDischargeSyntheticIncludeEntry =
          directive.removable &&
          isValidationDischargeableSyntheticIncludeEntry(directive.origin) &&
          directive.semanticsKnown && validRange && hasObserverRecord &&
          hasLayoutRecord && !hasConcreteObserverLiveComponent &&
          includeEntryObserversDominated;
      if (validationMayDischargeSyntheticIncludeEntry) {
        if (hasConcreteLayoutLiveComponent)
          addReason("synthetic include-entry has no undominated preserved "
                    "observer; final clang -E -P validation may discharge "
                    "layout-only liveness");
        else
          addReason("synthetic include-entry has no undominated preserved "
                    "observer/layout consumer; final clang -E -P validation "
                    "may discharge residual uncertainty");
      }

      const bool validationMayDischargePreIncludeNewlineResync =
          directive.removable &&
          directive.origin == FinalLineDirective::Origin::SyntheticNewlineResync &&
          directive.semanticsKnown && validRange && hasObserverRecord &&
          hasLayoutRecord && !hasConcreteObserverLiveComponent &&
          !hasConcreteLayoutLiveComponent &&
          syntheticNewlineResyncIsStaleBeforeSimpleInclude(model, i);
      if (validationMayDischargePreIncludeNewlineResync)
        addReason("synthetic newline-resync is immediately before a simple "
                  "literal #include and a later #line repairs the parent "
                  "stream before any preserved observer; final clang -E -P "
                  "validation may discharge it");

      const bool modelProvedCanRemove =
          directive.removable &&
          directive.activity == FinalLineDirectiveActivity::KnownActive &&
          directive.semanticsKnown && decision.observerDead &&
          decision.layoutDead && validRange;
      const bool canRemove =
          modelProvedCanRemove || validationMayDischargeSyntheticIncludeEntry ||
          validationMayDischargePreIncludeNewlineResync;

      if (canRemove) {
        const uint64_t removedBegin = directive.finalBegin;
        const uint64_t removedEnd = directive.finalEnd;
        const uint64_t removedSize = removedEnd - removedBegin;

        std::string candidateOutput = current;
        candidateOutput.erase(static_cast<size_t>(removedBegin),
                              static_cast<size_t>(removedSize));

        if (!validationCallback) {
          decision.verificationRejected = true;
          addReason("final pruning validation callback is unavailable; "
                    "reject deletion fail-closed");
          if (recordDecisions)
            result.decisions.push_back(std::move(decision));
          continue;
        }

        std::string validationReason;
        decision.verificationAttempted = true;
        if (!validationCallback(current, candidateOutput, validationReason)) {
          decision.verificationRejected = true;
          if (validationReason.empty())
            addReason("final clang -E -P validation rejected deletion");
          else
            addReason(formatv("final clang -E -P validation rejected "
                              "deletion: {0}",
                              validationReason)
                          .str());
          if (recordDecisions)
            result.decisions.push_back(std::move(decision));
          continue;
        }

        decision.removed = true;
        if (modelProvedCanRemove) {
          addReason("removed as explicit candidate with observer-dead, "
                    "layout-dead, and final clang -E -P equivalence proofs");
        } else if (validationMayDischargeSyntheticIncludeEntry) {
          addReason("removed as synthetic include-entry candidate with no "
                    "undominated preserved observer and final clang -E -P "
                    "equivalence proof");
        } else if (validationMayDischargePreIncludeNewlineResync) {
          addReason("removed as synthetic pre-include newline-resync candidate "
                    "with a downstream parent #line repair and final clang "
                    "-E -P equivalence proof");
        }
        if (recordDecisions)
          result.decisions.push_back(std::move(decision));

        current = std::move(candidateOutput);

        std::vector<FinalLineControlPruneCandidate> adjustedCandidates;
        adjustedCandidates.reserve(currentCandidates.size());
        for (FinalLineControlPruneCandidate candidate : currentCandidates) {
          if (candidate.finalBegin == removedBegin &&
              candidate.finalEnd == removedEnd)
            continue;

          if (candidate.finalEnd <= removedBegin) {
            adjustedCandidates.push_back(std::move(candidate));
            continue;
          }

          if (candidate.finalBegin >= removedEnd) {
            candidate.finalBegin -= removedSize;
            candidate.finalEnd -= removedSize;
            adjustedCandidates.push_back(std::move(candidate));
            continue;
          }

          // A candidate overlapping a removed directive but not exactly equal to
          // it no longer has a stable final-stream byte range.  Drop that
          // candidate so the next iteration fails closed rather than pruning a
          // shifted or partially removed directive by guesswork.
        }
        currentCandidates = std::move(adjustedCandidates);
        CanonicalizeFinalLineControlPruneCandidates(currentCandidates);

        AdjustFinalLineControlSourceMappingsAfterDeletion(
            currentSourceMappings, removedBegin, removedEnd);
        result.changed = true;
        removedThisIteration = true;
        break;
      }

      if (recordDecisions)
        result.decisions.push_back(std::move(decision));
    }

    ++result.iterations;
    if (!removedThisIteration)
      break;
  }

  result.output = std::move(current);
  return result;
}

void TraceFinalLineControlPruneResult(
    const FinalLineControlPruneResult &result, StringRef phase) {
  if (!inTraceMode())
    return;

  trace("line-prune", "{0}: fixed-point pruning changed={1} iterations={2} "
                       "decisions={3} outputBytes={4}",
        phase, result.changed, result.iterations, result.decisions.size(),
        result.output.size());

  for (const FinalLineControlPruneDecision &decision : result.decisions)
    trace("line-prune", "{0}: {1}", phase, decision);
}

void TraceFinalLineControlModel(const FinalLineControlModel &model,
                                StringRef phase) {
  if (!inTraceMode())
    return;

  std::vector<FinalLineDirectiveObserverLiveness> observerLiveness =
      ComputeFinalObserverLiveness(model);
  std::vector<FinalLineDirectiveLayoutLiveness> layoutLiveness =
      ComputeFinalLayoutLiveness(model);

  trace("line-prune", "{0}: final line-control facts: directives={1} "
                      "observers={2} observerLiveness={3} "
                      "layoutLiveness={4} layoutObligations={5} "
                      "physicalLines={6} sourceMappings={7}",
        phase, model.Directives().size(), model.Observers().size(),
        observerLiveness.size(), layoutLiveness.size(),
        model.LayoutObligations().size(), model.PhysicalLines().size(),
        model.SourceMappings().size());

  for (const FinalLineDirective &directive : model.Directives())
    trace("line-prune", "{0}: {1}", phase, directive);

  for (const FinalObserver &observer : model.Observers())
    trace("line-prune", "{0}: {1}", phase, observer);

  for (const FinalLineDirectiveObserverLiveness &liveness : observerLiveness)
    trace("line-prune", "{0}: {1}", phase, liveness);

  for (const FinalLineDirectiveLayoutLiveness &liveness : layoutLiveness)
    trace("line-prune", "{0}: {1}", phase, liveness);

  for (const FinalLayoutObligation &obligation : model.LayoutObligations())
    trace("line-prune", "{0}: {1}", phase, obligation);

  for (const FinalPhysicalLine &line : model.PhysicalLines())
    trace("line-prune", "{0}: {1}", phase, line);

  for (const FinalLineControlSourceMapping &mapping : model.SourceMappings())
    trace("line-prune", "{0}: {1}", phase, mapping);
}

} // namespace refold
} // namespace clang

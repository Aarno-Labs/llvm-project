//===--- FinalLineControlModel.cpp -----------------------------*- C++ -*-===//
//
// Final-stream line-control proof and pruning support for clang-refold.
//
// This file implements the compact proof carriers, source mappings, fixed-point
// candidate ordering, and validation-backed deletion machinery used to remove
// redundant synthetic line-control directives from the final emitted stream.
//
//===----------------------------------------------------------------------===//

#include "line-control/FinalLineControlModel.h"
#include "util/StringUtils.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>

using namespace llvm;

namespace clang {
namespace refold {

const char *toString(FinalLineControlObligation obligation) {
  switch (obligation) {
  case FinalLineControlObligation::SourceStateRepair:
    return "SourceStateRepair";
  case FinalLineControlObligation::IncludeReturnRepair:
    return "IncludeReturnRepair";
  case FinalLineControlObligation::TUPrologueRepair:
    return "TUPrologueRepair";
  case FinalLineControlObligation::HeaderResumeRepair:
    return "HeaderResumeRepair";
  case FinalLineControlObligation::LayoutBoundaryRepair:
    return "LayoutBoundaryRepair";
  case FinalLineControlObligation::BuiltinObserverLive:
    return "BuiltinObserverLive";
  case FinalLineControlObligation::CosmeticSyntheticResync:
    return "CosmeticSyntheticResync";
  case FinalLineControlObligation::DominatedSyntheticDirective:
    return "DominatedSyntheticDirective";
  }
  return "Unknown";
}

const char *toString(FinalLineControlRemovalVerdict verdict) {
  switch (verdict) {
  case FinalLineControlRemovalVerdict::NotProven:
    return "NotProven";
  case FinalLineControlRemovalVerdict::Removable:
    return "Removable";
  case FinalLineControlRemovalVerdict::Required:
    return "Required";
  }
  return "Unknown";
}

const char *toString(FinalLineControlRemovalDischarge discharge) {
  switch (discharge) {
  case FinalLineControlRemovalDischarge::None:
    return "None";
  case FinalLineControlRemovalDischarge::ObserverAndLayoutDead:
    return "ObserverAndLayoutDead";
  case FinalLineControlRemovalDischarge::SyntheticIncludeEntryDominated:
    return "SyntheticIncludeEntryDominated";
  case FinalLineControlRemovalDischarge::
      SyntheticNewlineResyncStaleBeforeInclude:
    return "SyntheticNewlineResyncStaleBeforeInclude";
  case FinalLineControlRemovalDischarge::SyntheticTUPrologueDominatedByRepair:
    return "SyntheticTUPrologueDominatedByRepair";
  case FinalLineControlRemovalDischarge::ValidationPreservedEquivalence:
    return "ValidationPreservedEquivalence";
  }
  return "Unknown";
}

const char *toString(FinalLineDirective::Origin origin) {
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
  return "Unknown";
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

  std::sort(mappings.begin(), mappings.end(),
            [](const FinalLineControlSourceMapping &lhs,
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
      const bool sameOwner = prev.physicalFile == mapping.physicalFile &&
                             prev.ownerIncludeId == mapping.ownerIncludeId;
      if (sameOwner && prev.finalBegin == mapping.finalBegin &&
          prev.finalEnd == mapping.finalEnd &&
          prev.sourceBegin == mapping.sourceBegin &&
          prev.sourceEnd == mapping.sourceEnd) {
        continue;
      }

      const bool contiguousFinal = prev.finalEnd == mapping.finalBegin;
      const bool contiguousSource = prev.sourceEnd == mapping.sourceBegin;
      if (contiguousFinal && contiguousSource && sameOwner) {
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
  adjusted.reserve(mappings.size());

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

    if (mapping.finalBegin < removedBegin) {
      FinalLineControlSourceMapping prefix = mapping;
      prefix.finalEnd = removedBegin;
      prefix.sourceEnd =
          mapping.sourceBegin + (removedBegin - mapping.finalBegin);
      adjusted.push_back(std::move(prefix));
    }

    if (mapping.finalEnd > removedEnd) {
      FinalLineControlSourceMapping suffix = mapping;
      suffix.sourceBegin =
          mapping.sourceEnd - (mapping.finalEnd - removedEnd);
      suffix.finalBegin = removedBegin;
      suffix.finalEnd = mapping.finalEnd - removedSize;
      adjusted.push_back(std::move(suffix));
    }
  }

  mappings = std::move(adjusted);
  CanonicalizeFinalLineControlSourceMappings(mappings);
}

bool SameFinalLineControlOwner(
    const std::optional<FinalLineControlOwnerKey> &lhs,
    const std::optional<FinalLineControlOwnerKey> &rhs) {
  if (lhs.has_value() != rhs.has_value())
    return false;
  if (!lhs)
    return true;
  return lhs->physicalFile == rhs->physicalFile &&
         lhs->ownerIncludeId == rhs->ownerIncludeId;
}

bool SameFinalLineControlObligationProof(
    const std::optional<FinalLineControlObligationProof> &lhs,
    const std::optional<FinalLineControlObligationProof> &rhs) {
  if (lhs.has_value() != rhs.has_value())
    return false;
  if (!lhs)
    return true;
  return lhs->obligation == rhs->obligation && lhs->origin == rhs->origin &&
         SameFinalLineControlOwner(lhs->physicalOwner, rhs->physicalOwner) &&
         lhs->producerProven == rhs->producerProven;
}

bool SameFinalLineControlRemovalProof(
    const std::optional<FinalLineControlRemovalProof> &lhs,
    const std::optional<FinalLineControlRemovalProof> &rhs) {
  if (lhs.has_value() != rhs.has_value())
    return false;
  if (!lhs)
    return true;
  return lhs->verdict == rhs->verdict && lhs->discharge == rhs->discharge &&
         lhs->origin == rhs->origin &&
         SameFinalLineControlOwner(lhs->physicalOwner, rhs->physicalOwner) &&
         lhs->producerProven == rhs->producerProven;
}

FinalLineControlObligationProof MakeFinalLineControlObligationProof(
    FinalLineControlObligation obligation, FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> physicalOwner,
    bool producerProven) {
  FinalLineControlObligationProof proof;
  proof.obligation = obligation;
  proof.origin = origin;
  proof.physicalOwner = std::move(physicalOwner);
  proof.producerProven = producerProven;
  return proof;
}

FinalLineControlRemovalProof MakeFinalLineControlRemovalProof(
    FinalLineControlRemovalVerdict verdict, FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> physicalOwner, bool producerProven,
    FinalLineControlRemovalDischarge discharge) {
  FinalLineControlRemovalProof proof;
  proof.verdict = verdict;
  proof.discharge = discharge;
  proof.origin = origin;
  proof.physicalOwner = std::move(physicalOwner);
  proof.producerProven = producerProven;
  return proof;
}

FinalLineControlPruneCandidate MakeFinalLineControlPruneCandidate(
    uint64_t finalBegin, uint64_t finalEnd,
    FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> physicalOwner, bool producerProven,
    FinalLineControlObligation obligation,
    FinalLineControlRemovalVerdict removalVerdict) {
  FinalLineControlPruneCandidate candidate;
  candidate.finalBegin = finalBegin;
  candidate.finalEnd = finalEnd;
  candidate.origin = origin;
  candidate.physicalOwner = std::move(physicalOwner);
  candidate.producerProven = producerProven;
  candidate.obligationProof = MakeFinalLineControlObligationProof(
      obligation, candidate.origin, candidate.physicalOwner,
      candidate.producerProven);
  candidate.removalProof = MakeFinalLineControlRemovalProof(
      removalVerdict, candidate.origin, candidate.physicalOwner,
      candidate.producerProven);
  return candidate;
}

bool HasCompleteFinalLineControlProof(
    const FinalLineControlPruneCandidate &candidate) {
  return candidate.obligationProof.has_value() &&
         candidate.removalProof.has_value();
}

namespace {

bool ownerLess(const std::optional<FinalLineControlOwnerKey> &lhs,
               const std::optional<FinalLineControlOwnerKey> &rhs) {
  if (lhs.has_value() != rhs.has_value())
    return !lhs.has_value();
  if (!lhs)
    return false;
  if (lhs->physicalFile != rhs->physicalFile)
    return lhs->physicalFile < rhs->physicalFile;
  return lhs->ownerIncludeId < rhs->ownerIncludeId;
}

/// Canonicalize removable final-line-control candidates so the fixed-point
/// pruner is independent of incidental emitter/vector ordering.
void CanonicalizeFinalLineControlPruneCandidates(
    std::vector<FinalLineControlPruneCandidate> &candidates) {
  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(),
                     [](const FinalLineControlPruneCandidate &candidate) {
                       return candidate.finalBegin >= candidate.finalEnd;
                     }),
      candidates.end());

  std::sort(candidates.begin(), candidates.end(),
            [](const FinalLineControlPruneCandidate &lhs,
               const FinalLineControlPruneCandidate &rhs) {
              if (lhs.finalBegin != rhs.finalBegin)
                return lhs.finalBegin < rhs.finalBegin;
              if (lhs.finalEnd != rhs.finalEnd)
                return lhs.finalEnd < rhs.finalEnd;
              if (lhs.origin != rhs.origin)
                return static_cast<unsigned>(lhs.origin) <
                       static_cast<unsigned>(rhs.origin);
              if (ownerLess(lhs.physicalOwner, rhs.physicalOwner))
                return true;
              if (ownerLess(rhs.physicalOwner, lhs.physicalOwner))
                return false;
              return !lhs.producerProven && rhs.producerProven;
            });

  std::vector<FinalLineControlPruneCandidate> canonical;
  canonical.reserve(candidates.size());
  for (FinalLineControlPruneCandidate candidate : candidates) {
    if (!canonical.empty() &&
        canonical.back().finalBegin == candidate.finalBegin &&
        canonical.back().finalEnd == candidate.finalEnd) {
      FinalLineControlPruneCandidate &merged = canonical.back();

      if (merged.origin != candidate.origin)
        merged.origin = FinalLineDirective::Origin::Unknown;
      if (!SameFinalLineControlOwner(merged.physicalOwner,
                                     candidate.physicalOwner))
        merged.physicalOwner = std::nullopt;
      if (!SameFinalLineControlObligationProof(merged.obligationProof,
                                               candidate.obligationProof))
        merged.obligationProof = std::nullopt;
      if (!SameFinalLineControlRemovalProof(merged.removalProof,
                                            candidate.removalProof))
        merged.removalProof = std::nullopt;

      merged.producerProven = merged.producerProven || candidate.producerProven;
      continue;
    }

    canonical.push_back(std::move(candidate));
  }

  candidates = std::move(canonical);
}

bool candidateRangeIsValid(const FinalLineControlPruneCandidate &candidate,
                           StringRef current) {
  return candidate.finalBegin < candidate.finalEnd &&
         candidate.finalEnd <= current.size();
}

StringRef preprocessorKeyword(StringRef line) {
  line = stringutils::trimHorizontal(line);
  if (!line.starts_with("#"))
    return StringRef();
  line = stringutils::trimHorizontal(line.drop_front());
  size_t end = 0;
  while (end < line.size() && stringutils::isIdentPart(line[end]))
    ++end;
  return line.take_front(end);
}

bool isConditionalDirectiveKeyword(StringRef keyword) {
  return keyword == "if" || keyword == "ifdef" || keyword == "ifndef" ||
         keyword == "elif" || keyword == "elifdef" || keyword == "elifndef" ||
         keyword == "else" || keyword == "endif";
}

bool isOpeningConditionalDirectiveKeyword(StringRef keyword) {
  return keyword == "if" || keyword == "ifdef" || keyword == "ifndef";
}

bool lineIsBlankOrCommentOnly(StringRef line) {
  line = stringutils::trimHorizontal(line);
  return line.empty() || line.starts_with("//") || line.starts_with("/*") ||
         line.starts_with("*");
}

bool lineDirectiveUsesSingleMacroBundle(StringRef line) {
  line = stringutils::trimHorizontal(line);
  if (!line.starts_with("#"))
    return false;
  line = stringutils::trimHorizontal(line.drop_front());
  if (!line.consume_front("line"))
    return false;
  line = stringutils::trimHorizontal(line);
  if (line.empty() || !stringutils::isIdentStart(line.front()))
    return false;

  size_t identEnd = 1;
  while (identEnd < line.size() && stringutils::isIdentPart(line[identEnd]))
    ++identEnd;
  line = stringutils::trimHorizontal(line.drop_front(identEnd));

  // A single macro operand can expand to the complete GNU/Clang accepted
  // `#line` operand bundle: numeric line plus optional filename.  Separate
  // operands such as `LOC_LINE LOC_FILE`, literals, or builtins are already
  // explicit in the preserved source and do not require this prologue guard.
  return line.empty() || line.starts_with("//") || line.starts_with("/*");
}

bool suffixContainsPreservedFileBuiltin(StringRef text, size_t offset) {
  StringRef suffix = text.drop_front(std::min(offset, text.size()));
  return suffix.contains("__FILE__") || suffix.contains("__FILE_NAME__");
}

std::optional<StringRef> nextPreprocessorKeywordAfter(StringRef text,
                                                      uint64_t offset) {
  size_t cursor = static_cast<size_t>(std::min<uint64_t>(offset, text.size()));
  while (cursor < text.size()) {
    size_t lineEnd = text.find('\n', cursor);
    if (lineEnd == StringRef::npos)
      lineEnd = text.size();
    StringRef line = text.slice(cursor, lineEnd);
    StringRef keyword = preprocessorKeyword(line);
    if (!keyword.empty())
      return keyword;
    if (!lineIsBlankOrCommentOnly(line))
      return std::nullopt;
    cursor = lineEnd == text.size() ? text.size() : lineEnd + 1;
  }
  return std::nullopt;
}

bool offsetIsInsideConditionalSourceRegion(StringRef text, uint64_t offset) {
  uint64_t limit = std::min<uint64_t>(offset, text.size());
  unsigned depth = 0;
  size_t cursor = 0;
  while (cursor < limit) {
    size_t lineEnd = text.find('\n', cursor);
    if (lineEnd == StringRef::npos)
      lineEnd = text.size();
    StringRef keyword = preprocessorKeyword(text.slice(cursor, lineEnd));
    if (isOpeningConditionalDirectiveKeyword(keyword)) {
      ++depth;
    } else if (keyword == "endif") {
      if (depth > 0)
        --depth;
    }
    cursor = lineEnd == text.size() ? text.size() : lineEnd + 1;
  }
  return depth != 0;
}

bool tuPrologueProtectsConditionalLineControl(StringRef text,
                                              uint64_t afterDirective) {
  unsigned depth = 0;
  size_t cursor =
      static_cast<size_t>(std::min<uint64_t>(afterDirective, text.size()));
  while (cursor < text.size()) {
    size_t lineEnd = text.find('\n', cursor);
    if (lineEnd == StringRef::npos)
      lineEnd = text.size();
    StringRef line = text.slice(cursor, lineEnd);
    StringRef keyword = preprocessorKeyword(line);

    if (isOpeningConditionalDirectiveKeyword(keyword)) {
      ++depth;
    } else if (keyword == "endif") {
      if (depth > 0)
        --depth;
    } else if (keyword == "line" && depth != 0) {
      // A source-authored line-control directive inside a preserved conditional
      // arm is configuration-local state.  The TU prologue is the compact
      // source-state witness that lets replay enter that conditional island from
      // the original TU coordinate space; executable -E -P validation alone is
      // not allowed to erase it.
      return true;
    } else if (keyword == "line" && depth == 0 &&
               lineDirectiveUsesSingleMacroBundle(line) &&
               suffixContainsPreservedFileBuiltin(text, lineEnd)) {
      // A top-level #line whose complete operand bundle is imported through one
      // macro can change both line and file state without spelling the filename
      // in the TU.  When a preserved file builtin remains in the suffix, the TU
      // prologue is the only compact witness that replay entered the source file
      // before that imported state was applied.
      return true;
    } else if (keyword.empty() && !lineIsBlankOrCommentOnly(line)) {
      // Once ordinary source text is reached before any conditional #line,
      // this local guard has no proof obligation to preserve.
      return false;
    }

    cursor = lineEnd == text.size() ? text.size() : lineEnd + 1;
  }
  return false;
}

bool candidateCarriesRequiredSourceState(
    const FinalLineControlPruneCandidate &candidate, StringRef current) {
  if (!candidate.obligationProof)
    return true;

  const FinalLineControlObligation obligation =
      candidate.obligationProof->obligation;

  if (candidate.origin == FinalLineDirective::Origin::SyntheticTUPrologue &&
      obligation == FinalLineControlObligation::TUPrologueRepair) {
    return tuPrologueProtectsConditionalLineControl(current,
                                                    candidate.finalEnd);
  }

  if (candidate.origin == FinalLineDirective::Origin::SyntheticIncludeReturn &&
      obligation == FinalLineControlObligation::IncludeReturnRepair) {
    // Include-return repairs inside a preserved conditional arm are not merely
    // layout cosmetics: they re-enter the parent owner before arm-local source
    // text can observe line state.  A later join repair may be necessary for
    // other configurations, but it does not dominate this selected-arm witness.
    return offsetIsInsideConditionalSourceRegion(current, candidate.finalBegin);
  }

  if (candidate.origin == FinalLineDirective::Origin::SyntheticNewlineResync &&
      obligation == FinalLineControlObligation::CosmeticSyntheticResync) {
    std::optional<StringRef> nextKeyword =
        nextPreprocessorKeywordAfter(current, candidate.finalEnd);
    if (nextKeyword && isConditionalDirectiveKeyword(*nextKeyword) &&
        candidate.physicalOwner && candidate.physicalOwner->ownerIncludeId) {
      // Header-local newline resyncs immediately before preserved conditional
      // directive structure carry source-state shape even when deleting them is
      // token-equivalent under the active configuration.
      return true;
    }
  }

  return false;
}

bool candidateMayBeValidationDischarged(
    const FinalLineControlPruneCandidate &candidate, StringRef current) {
  if (!candidateRangeIsValid(candidate, current))
    return false;
  if (!HasCompleteFinalLineControlProof(candidate))
    return false;
  if (candidate.removalProof->verdict ==
      FinalLineControlRemovalVerdict::Required)
    return false;
  if (candidateCarriesRequiredSourceState(candidate, current))
    return false;
  return true;
}

void markCandidateValidationDischarged(
    FinalLineControlPruneCandidate &candidate) {
  candidate.removalProof = MakeFinalLineControlRemovalProof(
      FinalLineControlRemovalVerdict::Removable, candidate.origin,
      candidate.physicalOwner, candidate.producerProven,
      FinalLineControlRemovalDischarge::ValidationPreservedEquivalence);
}

void adjustCandidatesAfterDeletion(
    std::vector<FinalLineControlPruneCandidate> &candidates,
    uint64_t removedBegin, uint64_t removedEnd) {
  const uint64_t removedSize = removedEnd - removedBegin;
  std::vector<FinalLineControlPruneCandidate> adjusted;
  adjusted.reserve(candidates.size());

  for (FinalLineControlPruneCandidate candidate : candidates) {
    if (candidate.finalBegin == removedBegin &&
        candidate.finalEnd == removedEnd)
      continue;

    if (candidate.finalEnd <= removedBegin) {
      adjusted.push_back(std::move(candidate));
      continue;
    }

    if (candidate.finalBegin >= removedEnd) {
      candidate.finalBegin -= removedSize;
      candidate.finalEnd -= removedSize;
      adjusted.push_back(std::move(candidate));
      continue;
    }

    // A candidate overlapping a removed directive but not exactly equal to it
    // no longer has a stable final-stream byte range.  Drop it fail-closed
    // rather than pruning a shifted or partially removed directive by
    // guesswork.
  }

  candidates = std::move(adjusted);
  CanonicalizeFinalLineControlPruneCandidates(candidates);
}

} // namespace

FinalLineControlAuthorityContract GetFinalLineControlAuthorityContract() {
  return FinalLineControlAuthorityContract();
}

FinalLineControlPruneResult PruneFinalLineControlDirectives(
    StringRef finalSource,
    ArrayRef<FinalLineControlPruneCandidate> removableCandidates,
    FinalLineControlValidationCallback validationCallback) {
  FinalLineControlPruneResult result;
  result.authority = GetFinalLineControlAuthorityContract();

  std::string current = finalSource.str();
  std::vector<FinalLineControlPruneCandidate> currentCandidates(
      removableCandidates.begin(), removableCandidates.end());
  CanonicalizeFinalLineControlPruneCandidates(currentCandidates);

  for (;;) {
    bool removedThisIteration = false;

    for (FinalLineControlPruneCandidate &candidate : currentCandidates) {
      if (!candidateMayBeValidationDischarged(candidate, current)) {
        continue;
      }

      const uint64_t removedBegin = candidate.finalBegin;
      const uint64_t removedEnd = candidate.finalEnd;
      const uint64_t removedSize = removedEnd - removedBegin;

      std::string candidateOutput = current;
      candidateOutput.erase(static_cast<size_t>(removedBegin),
                            static_cast<size_t>(removedSize));

      bool validationAcceptedDeletion = false;
      if (validationCallback) {
        std::string validationReason;
        validationAcceptedDeletion =
            validationCallback(current, candidateOutput, validationReason);
      }

      if (!validationAcceptedDeletion) {
        continue;
      }

      // The exact byte deletion has now been discharged by the executable
      // oracle. Store that theorem fact before mutating the stream, so every
      // surviving removal is represented by the compact proof carrier rather
      // than by legacy scanner side state.
      markCandidateValidationDischarged(candidate);
      result.removedRanges.push_back({removedBegin, removedEnd});
      current = std::move(candidateOutput);
      adjustCandidatesAfterDeletion(currentCandidates, removedBegin,
                                    removedEnd);

      result.changed = true;
      removedThisIteration = true;
      break;
    }

    ++result.iterations;
    if (!removedThisIteration)
      break;
  }

  result.output = std::move(current);
  return result;
}

} // namespace refold
} // namespace clang

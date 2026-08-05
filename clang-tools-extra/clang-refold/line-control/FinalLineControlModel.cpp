//===--- FinalLineControlModel.cpp ------------------------------*- C++ -*-===//
//
// Final-stream line-control proof and pruning support for clang-refold.
//
// This file implements the compact proof carriers, source mappings,
// fixed-point candidate ordering, and validation-backed deletion machinery
// used to remove redundant synthetic line-control directives from the final
// emitted stream.  It also implements `buildFinalLineControlValidationCallback`
// and its two static helpers (`writePruneValidationSource`,
// `preprocessedTokensEqualForLinePrune`), which together produce the
// executable preprocessing oracle the pruner consults for each proposed
// deletion.
//
//===----------------------------------------------------------------------===//

#include "line-control/FinalLineControlModel.h"

#include "core/RefoldLangOptions.h"
#include "core/RefoldPreprocessRecheck.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldToken.h"
#include "util/StringUtils.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

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
  case FinalLineControlRemovalDischarge::ValidationPreservedEquivalence:
    return "ValidationPreservedEquivalence";
  }
  return "Unknown";
}

const char *toString(FinalLineDirective::Origin origin) {
  switch (origin) {
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
      suffix.sourceBegin = mapping.sourceEnd - (mapping.finalEnd - removedEnd);
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
    uint64_t finalBegin, uint64_t finalEnd, FinalLineDirective::Origin origin,
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
      // source-state witness that lets replay enter that conditional island
      // from the original TU coordinate space; executable -E -P validation
      // alone is not allowed to erase it.
      return true;
    } else if (keyword == "line" && depth == 0 &&
               lineDirectiveUsesSingleMacroBundle(line) &&
               suffixContainsPreservedFileBuiltin(text, lineEnd)) {
      // A top-level #line whose complete operand bundle is imported through one
      // macro can change both line and file state without spelling the filename
      // in the TU.  When a preserved file builtin remains in the suffix, the TU
      // prologue is the only compact witness that replay entered the source
      // file before that imported state was applied.
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

FinalLineControlAuthorityContract getFinalLineControlAuthorityContract() {
  return FinalLineControlAuthorityContract();
}

FinalLineControlPruneResult PruneFinalLineControlDirectives(
    StringRef finalSource,
    ArrayRef<FinalLineControlPruneCandidate> removableCandidates,
    FinalLineControlValidationCallback validationCallback) {
  FinalLineControlPruneResult result;
  result.authority = getFinalLineControlAuthorityContract();

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

static Error writePruneValidationSource(StringRef path, StringRef bytes) {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_Text);
  if (ec)
    return createStringError(
        ec, formatv("cannot write pruning validation source '{0}'", path));
  os << bytes;
  os.close();
  if (os.has_error())
    return createStringError(
        os.error(),
        formatv("failed to flush pruning validation source '{0}'", path));
  return Error::success();
}

/// Return true iff two already-preprocessed `-E -P` byte streams have the same
/// token sequence.
///
/// Final line-control pruning normally requires byte-for-byte preprocessing
/// equivalence.  That is intentionally stronger than the refolding checker, but
/// it is too strong for stale `#line` directives whose only remaining effect is
/// Clang's cosmetic blank-line accounting around zero-token/comment-only source
/// lines.  In those cases the correct semantic oracle is the same one used by
/// `--check`: the emitted source must replay to the same preprocessed token
/// stream, including any materialized `__LINE__`, `__FILE__`, and
/// `__FILE_NAME__` expansions.
static bool
preprocessedTokensEqualForLinePrune(StringRef currentPP, StringRef candidatePP,
                                    const RefoldModel::PreprocessContext &ctx,
                                    std::string &reason) {
  const LangOptions lexLang = makeRefoldLexLangOptions(ctx.lang);
  std::vector<PPTok> currentTokens;
  std::vector<PPTok> candidateTokens;
  std::vector<std::size_t> currentOffsets;
  std::vector<std::size_t> candidateOffsets;

  lexPPTokens(currentPP.str(), currentTokens, currentOffsets, lexLang);
  lexPPTokens(candidatePP.str(), candidateTokens, candidateOffsets, lexLang);

  const size_t n = std::min(currentTokens.size(), candidateTokens.size());
  for (size_t i = 0; i < n; ++i) {
    if (currentTokens[i].spelling == candidateTokens[i].spelling)
      continue;

    reason = formatv("preprocessed bytes differ and token streams differ at "
                     "index {0}: current='{1}' candidate='{2}'",
                     i,
                     stringutils::showWs(stringutils::clip(
                         StringRef(currentTokens[i].spelling), 100)),
                     stringutils::showWs(stringutils::clip(
                         StringRef(candidateTokens[i].spelling), 100)))
                 .str();
    return false;
  }

  if (currentTokens.size() != candidateTokens.size()) {
    reason = formatv("preprocessed bytes differ and token counts differ: "
                     "current={0} candidate={1}",
                     currentTokens.size(), candidateTokens.size())
                 .str();
    return false;
  }

  reason.clear();
  return true;
}

std::optional<std::string>
producerSourceAnchorPath(StringRef producerSourcePath,
                         const RefoldModel::PreprocessContext &ctx) {
  if (producerSourcePath.empty())
    return std::nullopt;

  SmallString<256> anchor(producerSourcePath);
  if (sys::path::is_absolute(anchor))
    return anchor.str().str();

  // A relative `source` is spelled relative to the producer working directory,
  // which is the same directory `preprocessToBytes` re-enters.
  if (ctx.cwd.empty())
    return std::nullopt;
  SmallString<256> resolved(ctx.cwd);
  sys::path::append(resolved, producerSourcePath);
  return resolved.str().str();
}

/// Create an internal final-pruning validation callback.
///
/// The callback does not implement a user-facing `--check` mode.  It is an
/// executable guard for one proposed `#line` deletion: preprocess the current
/// accepted final source and the candidate final source through the same
/// `clang -E -P` context, using one stable temporary source path located beside
/// the producer's own source.  Reusing the same source path for both inputs
/// keeps `__FILE__` and quoted-include lookup comparable, and anchoring on the
/// producer's directory is what makes a quoted include resolve to the header the
/// producer actually read.  Byte-for-byte equality is accepted first; if the
/// only difference is preprocessing trivia, token-sequence equality is also
/// accepted because the refolding soundness oracle is token equivalence.
FinalSourcePreprocessCallback
buildFinalSourcePreprocessCallback(StringRef anchorPath,
                                   const RefoldModel::PreprocessContext &ctx,
                                   ArrayRef<std::string> verifyIncludeDirs) {
  SmallString<256> outputDir(anchorPath);
  sys::path::remove_filename(outputDir);
  if (outputDir.empty())
    outputDir = ".";

  SmallString<256> model(outputDir);
  sys::path::append(model, ".clang-refold-observer-audit-%%%%%%.c");
  std::string modelText = model.str().str();

  std::vector<std::string> extraArgs;
  extraArgs.reserve(verifyIncludeDirs.size() * 2);
  for (const std::string &dir : verifyIncludeDirs) {
    extraArgs.push_back("-I");
    extraArgs.push_back(dir);
  }

  return [modelText, ctx,
          extraArgs](StringRef finalSource) -> std::optional<std::string> {
    SmallString<256> tmpPath;
    int tmpFD = -1;
    if (sys::fs::createUniqueFile(modelText, tmpFD, tmpPath))
      return std::nullopt;
    {
      raw_fd_ostream closeStream(tmpFD, /*shouldClose=*/true);
      closeStream.close();
    }
    auto cleanup = make_scope_exit([&]() { (void)sys::fs::remove(tmpPath); });

    if (Error err = writePruneValidationSource(tmpPath, finalSource)) {
      consumeError(std::move(err));
      return std::nullopt;
    }
    auto ppOrErr = preprocessToBytes(tmpPath, ctx, extraArgs);
    if (!ppOrErr) {
      consumeError(ppOrErr.takeError());
      return std::nullopt;
    }
    return std::move(*ppOrErr);
  };
}

FinalLineControlValidationCallback buildFinalLineControlValidationCallback(
    StringRef outputPath, const RefoldModel::PreprocessContext &ctx) {
  SmallString<256> outputDir(outputPath);
  sys::path::remove_filename(outputDir);
  if (outputDir.empty())
    outputDir = ".";

  SmallString<256> model(outputDir);
  sys::path::append(model, ".clang-refold-line-prune-%%%%%%.c");
  std::string modelText = model.str().str();

  return [modelText, ctx](StringRef currentOutput, StringRef candidateOutput,
                          std::string &reason) -> bool {
    SmallString<256> tmpPath;
    int tmpFD = -1;
    if (std::error_code ec =
            sys::fs::createUniqueFile(modelText, tmpFD, tmpPath)) {
      reason = formatv("could not create pruning validation source '{0}': {1}",
                       modelText, ec.message())
                   .str();
      return false;
    }

    {
      raw_fd_ostream closeStream(tmpFD, /*shouldClose=*/true);
      closeStream.close();
    }

    auto cleanup = make_scope_exit([&]() { (void)sys::fs::remove(tmpPath); });

    if (Error err = writePruneValidationSource(tmpPath, currentOutput)) {
      reason = toString(std::move(err));
      return false;
    }

    auto currentPPOrErr = preprocessToBytes(tmpPath, ctx);
    if (!currentPPOrErr) {
      reason = formatv("failed to preprocess current final source: {0}",
                       toString(currentPPOrErr.takeError()))
                   .str();
      return false;
    }

    if (Error err = writePruneValidationSource(tmpPath, candidateOutput)) {
      reason = toString(std::move(err));
      return false;
    }

    auto candidatePPOrErr = preprocessToBytes(tmpPath, ctx);
    if (!candidatePPOrErr) {
      reason = formatv("failed to preprocess candidate final source: {0}",
                       toString(candidatePPOrErr.takeError()))
                   .str();
      return false;
    }

    if (*currentPPOrErr != *candidatePPOrErr) {
      if (!preprocessedTokensEqualForLinePrune(*currentPPOrErr,
                                               *candidatePPOrErr, ctx, reason))
        return false;
      reason.clear();
      return true;
    }

    reason.clear();
    return true;
  };
}

bool AuditFinalLineControlAuthorityContract(
    const RefoldTheoremAudit &audit,
    const FinalLineControlAuthorityContract &authority, StringRef role) {
  if (!audit.IsNoLegacyAuditEnabled())
    return true;

  auto report = [&](StringRef detail) {
    audit.ReportNoLegacyAuditFinding(
        RefoldTheoremAudit::MakeLegacyAuditEvidence(
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

bool AuditFinalLineControlRemovalProofPopulation(
    const RefoldTheoremAudit &audit,
    ArrayRef<FinalLineControlPruneCandidate> candidates, StringRef role) {
  if (!audit.IsNoLegacyAuditEnabled())
    return true;

  size_t missing = 0;
  for (const FinalLineControlPruneCandidate &candidate : candidates)
    if (!HasCompleteFinalLineControlProof(candidate))
      ++missing;

  if (missing == 0)
    return true;

  audit.ReportNoLegacyAuditFinding(RefoldTheoremAudit::MakeLegacyAuditEvidence(
      LegacyPathKind::FinalLineControlLivenessWithoutObligation, role,
      llvm::formatv(
          "{0} final line-control prune candidate(s) lack compact "
          "obligation/removal proof; generation sites must populate both "
          "facts before compact final-line-control pruning may run",
          missing)
          .str()));
  return false;
}

} // namespace refold
} // namespace clang

//===--- RefoldPreprocessingStructureIndex.cpp ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exact preprocessing-structure census implementation.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldPreprocessingStructureIndex.h"

#include "source/RefoldPreprocessingDirectiveScanner.h"

#include "core/RefoldModel.h"
#include "macro/RefoldMacroStateProof.h"
#include "util/RefoldPathIdentity.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

StringRef toString(PreprocessingStructureKind kind) {
  switch (kind) {
  case PreprocessingStructureKind::ConditionalIf:
    return "ConditionalIf";
  case PreprocessingStructureKind::ConditionalIfdef:
    return "ConditionalIfdef";
  case PreprocessingStructureKind::ConditionalIfndef:
    return "ConditionalIfndef";
  case PreprocessingStructureKind::ConditionalElif:
    return "ConditionalElif";
  case PreprocessingStructureKind::ConditionalElifdef:
    return "ConditionalElifdef";
  case PreprocessingStructureKind::ConditionalElifndef:
    return "ConditionalElifndef";
  case PreprocessingStructureKind::ConditionalElse:
    return "ConditionalElse";
  case PreprocessingStructureKind::ConditionalEndif:
    return "ConditionalEndif";
  case PreprocessingStructureKind::MacroDefine:
    return "MacroDefine";
  case PreprocessingStructureKind::MacroUndef:
    return "MacroUndef";
  case PreprocessingStructureKind::Include:
    return "Include";
  case PreprocessingStructureKind::IncludeNext:
    return "IncludeNext";
  case PreprocessingStructureKind::Import:
    return "Import";
  case PreprocessingStructureKind::Pragma:
    return "Pragma";
  case PreprocessingStructureKind::PragmaOperator:
    return "PragmaOperator";
  case PreprocessingStructureKind::LineControl:
    return "LineControl";
  case PreprocessingStructureKind::ErrorDirective:
    return "ErrorDirective";
  case PreprocessingStructureKind::WarningDirective:
    return "WarningDirective";
  case PreprocessingStructureKind::OtherDirective:
    return "OtherDirective";
  }
  llvm_unreachable("Invalid preprocessing-structure kind");
}

StringRef toString(PreprocessingStructureModelKind kind) {
  switch (kind) {
  case PreprocessingStructureModelKind::None:
    return "None";
  case PreprocessingStructureModelKind::MacroDirective:
    return "MacroDirective";
  case PreprocessingStructureModelKind::IncludeDirective:
    return "IncludeDirective";
  case PreprocessingStructureModelKind::PragmaDirective:
    return "PragmaDirective";
  case PreprocessingStructureModelKind::LineControlEvent:
    return "LineControlEvent";
  case PreprocessingStructureModelKind::ConditionalDirective:
    return "ConditionalDirective";
  }
  llvm_unreachable("Invalid preprocessing-structure model kind");
}

namespace {

/// Lexically discovered preprocessing directive plus temporary binding state.
struct ScannedDirective {
  PreprocessingStructureInterval interval;
  uint64_t introducerBegin = 0;
  std::optional<size_t> lexicalOwnerArmIndex;
  bool modelBindingAmbiguous = false;
};

/// One lexically balanced conditional group and its arm-control directives.
struct ScannedConditionalGroup {
  size_t openingDirectiveIndex = 0;
  std::vector<size_t> armDirectiveIndices;
  size_t endifDirectiveIndex = 0;
  uint64_t begin = 0;
  uint64_t end = 0;
};

/// Open conditional stack frame used while building lexical group topology.
struct OpenConditionalGroup {
  size_t openingDirectiveIndex = 0;
  std::vector<size_t> armDirectiveIndices;
  size_t currentArmDirectiveIndex = 0;
};

/// Return whether a producer record belongs to the concrete indexed owner.
static bool ownerMatches(std::optional<uint64_t> recordOwner,
                         std::optional<uint64_t> requestedOwner) {
  return recordOwner == requestedOwner;
}

/// Return a stable diagnostic spelling for one physical source-owner domain.
static std::string
ownerDomainDescription(std::optional<uint64_t> ownerIncludeId) {
  if (!ownerIncludeId)
    return "TU";
  return llvm::formatv("include:{0}", *ownerIncludeId).str();
}

/// Return whether `[begin,end)` is one nonempty range in `sourceBytes`.
static bool sourceRangeValid(StringRef sourceBytes, uint64_t begin,
                             uint64_t end) {
  return begin < end && end <= sourceBytes.size();
}

/// Exact source range recovered from producer text and its callback bounds.
struct ExactProducerTextRange {
  uint64_t begin = 0;
  uint64_t end = 0;
};

/// Recover the exact physical source range named by producer directive text.
///
/// Some preprocessor callbacks report a site end before the complete logical
/// directive spelling.  The producer also serializes the directive text, so
/// the index can extend that narrow callback range only by an exact byte-for-
/// byte match starting at the recorded site beginning.  A callback end beyond
/// the matched text, an overflow, or any source-text mismatch rejects the
/// binding instead of guessing which surrounding bytes belong to it.
static std::optional<ExactProducerTextRange>
recoverExactProducerTextRange(StringRef sourceBytes, uint64_t begin,
                              uint64_t recordedEnd,
                              StringRef producerText) {
  if (begin >= recordedEnd || recordedEnd > sourceBytes.size() ||
      producerText.empty())
    return std::nullopt;
  if (producerText.size() >
      std::numeric_limits<uint64_t>::max() - begin)
    return std::nullopt;

  const uint64_t exactEnd = begin + producerText.size();
  if (exactEnd > sourceBytes.size() || recordedEnd > exactEnd)
    return std::nullopt;
  if (sourceBytes.slice(begin, exactEnd) != producerText)
    return std::nullopt;
  return ExactProducerTextRange{begin, exactEnd};
}

/// Return whether a scanned directive has the required lexical class.
static bool isDirectiveKind(PreprocessingStructureKind actual,
                            PreprocessingStructureKind expected) {
  return actual == expected;
}

/// Return whether `kind` opens a conditional group and its first arm.
static bool isConditionalOpening(PreprocessingStructureKind kind) {
  return kind == PreprocessingStructureKind::ConditionalIf ||
         kind == PreprocessingStructureKind::ConditionalIfdef ||
         kind == PreprocessingStructureKind::ConditionalIfndef;
}

/// Return whether `kind` switches the current arm of an open group.
static bool isConditionalMiddle(PreprocessingStructureKind kind) {
  return kind == PreprocessingStructureKind::ConditionalElif ||
         kind == PreprocessingStructureKind::ConditionalElifdef ||
         kind == PreprocessingStructureKind::ConditionalElifndef ||
         kind == PreprocessingStructureKind::ConditionalElse;
}

/// Return whether `kind` participates in conditional-control topology.
static bool isConditionalControl(PreprocessingStructureKind kind) {
  return isConditionalOpening(kind) || isConditionalMiddle(kind) ||
         kind == PreprocessingStructureKind::ConditionalEndif;
}

/// Classify one normalized preprocessing-directive head token.
static PreprocessingStructureKind classifyDirectiveKeyword(StringRef keyword,
                                                            bool numericHead) {
  if (numericHead)
    return PreprocessingStructureKind::LineControl;
  if (keyword == "if")
    return PreprocessingStructureKind::ConditionalIf;
  if (keyword == "ifdef")
    return PreprocessingStructureKind::ConditionalIfdef;
  if (keyword == "ifndef")
    return PreprocessingStructureKind::ConditionalIfndef;
  if (keyword == "elif")
    return PreprocessingStructureKind::ConditionalElif;
  if (keyword == "elifdef")
    return PreprocessingStructureKind::ConditionalElifdef;
  if (keyword == "elifndef")
    return PreprocessingStructureKind::ConditionalElifndef;
  if (keyword == "else")
    return PreprocessingStructureKind::ConditionalElse;
  if (keyword == "endif")
    return PreprocessingStructureKind::ConditionalEndif;
  if (keyword == "define")
    return PreprocessingStructureKind::MacroDefine;
  if (keyword == "undef")
    return PreprocessingStructureKind::MacroUndef;
  if (keyword == "include")
    return PreprocessingStructureKind::Include;
  if (keyword == "include_next")
    return PreprocessingStructureKind::IncludeNext;
  if (keyword == "import")
    return PreprocessingStructureKind::Import;
  if (keyword == "pragma")
    return PreprocessingStructureKind::Pragma;
  if (keyword == "line")
    return PreprocessingStructureKind::LineControl;
  if (keyword == "error")
    return PreprocessingStructureKind::ErrorDirective;
  if (keyword == "warning")
    return PreprocessingStructureKind::WarningDirective;
  return PreprocessingStructureKind::OtherDirective;
}

/// Return the end of the directive spelling, excluding only the final
/// unspliced physical newline included by the shared scanner.
static uint64_t directiveSpellingEnd(StringRef sourceBytes, uint64_t lineEnd) {
  if (lineEnd == 0 || lineEnd > sourceBytes.size())
    return lineEnd;
  if (sourceBytes[lineEnd - 1] == '\n') {
    if (lineEnd >= 2 && sourceBytes[lineEnd - 2] == '\r')
      return lineEnd - 2;
    return lineEnd - 1;
  }
  if (sourceBytes[lineEnd - 1] == '\r')
    return lineEnd - 1;
  return lineEnd;
}

/// Convert the shared exact lexical scan into structure-index intervals.
///
/// The shared scanner is the sole authority for directive recognition.  This
/// layer performs only semantic classification and producer binding; it never
/// reparses physical lines or searches for directive-looking text.
static std::vector<ScannedDirective>
scanPreprocessingStructure(StringRef sourcePath, StringRef sourceBytes,
                           std::optional<uint64_t> ownerIncludeId,
                           const LangOptions &lexLang,
                           std::vector<PreprocessingLexicalTokenInterval>
                               &lexicalTokenIntervals,
                           std::vector<PreprocessingTriviaInterval>
                               &triviaIntervals,
                           std::vector<PreprocessingIndivisibleTriviaInterval>
                               &indivisibleTriviaIntervals,
                           std::vector<std::string> &diagnostics) {
  PreprocessingDirectiveScanResult lexicalScan =
      scanPreprocessingDirectives(sourceBytes, lexLang);
  lexicalTokenIntervals = std::move(lexicalScan.lexicalTokenIntervals);
  triviaIntervals = std::move(lexicalScan.triviaIntervals);
  indivisibleTriviaIntervals =
      std::move(lexicalScan.indivisibleTriviaIntervals);
  diagnostics.insert(diagnostics.end(), lexicalScan.diagnostics.begin(),
                     lexicalScan.diagnostics.end());

  std::vector<ScannedDirective> directives;
  directives.reserve(lexicalScan.directives.size() +
                     lexicalScan.pragmaOperators.size());
  for (const PreprocessingDirectiveLine &lexicalDirective :
       lexicalScan.directives) {
    ScannedDirective scanned;
    const bool numericHead =
        lexicalDirective.headKind == PreprocessingDirectiveHeadKind::Numeric;
    scanned.interval.kind = classifyDirectiveKeyword(
        lexicalDirective.keyword, numericHead);
    scanned.interval.sourcePath = sourcePath.str();
    scanned.interval.ownerIncludeId = ownerIncludeId;
    scanned.interval.begin = lexicalDirective.begin;
    scanned.interval.end = lexicalDirective.end;
    scanned.interval.structureSpellingBegin =
        lexicalDirective.introducerBegin;
    scanned.interval.structureSpellingEnd =
        directiveSpellingEnd(sourceBytes, lexicalDirective.end);
    scanned.introducerBegin = lexicalDirective.introducerBegin;
    directives.push_back(std::move(scanned));
  }

  for (const PreprocessingPragmaOperatorInterval &lexicalOperator :
       lexicalScan.pragmaOperators) {
    ScannedDirective scanned;
    scanned.interval.kind = PreprocessingStructureKind::PragmaOperator;
    scanned.interval.sourcePath = sourcePath.str();
    scanned.interval.ownerIncludeId = ownerIncludeId;
    scanned.interval.begin = lexicalOperator.begin;
    scanned.interval.end = lexicalOperator.end;
    scanned.interval.structureSpellingBegin = lexicalOperator.begin;
    scanned.interval.structureSpellingEnd = lexicalOperator.end;
    scanned.introducerBegin = lexicalOperator.begin;
    directives.push_back(std::move(scanned));
  }

  return directives;
}

/// Deterministic physical-source ordering for scanned preprocessing structure.
static bool scannedDirectiveLess(const ScannedDirective &lhs,
                                 const ScannedDirective &rhs) {
  if (lhs.interval.begin != rhs.interval.begin)
    return lhs.interval.begin < rhs.interval.begin;
  if (lhs.interval.end != rhs.interval.end)
    return lhs.interval.end < rhs.interval.end;
  if (lhs.interval.kind != rhs.interval.kind)
    return static_cast<unsigned>(lhs.interval.kind) <
           static_cast<unsigned>(rhs.interval.kind);
  if (lhs.interval.modelKind != rhs.interval.modelKind)
    return static_cast<unsigned>(lhs.interval.modelKind) <
           static_cast<unsigned>(rhs.interval.modelKind);
  return lhs.interval.modelItemId < rhs.interval.modelItemId;
}

/// Deterministic physical-source ordering for balanced conditional groups.
static bool scannedConditionalGroupLess(const ScannedConditionalGroup &lhs,
                                        const ScannedConditionalGroup &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  if (lhs.end != rhs.end)
    return lhs.end < rhs.end;
  return lhs.openingDirectiveIndex < rhs.openingDirectiveIndex;
}

/// Balance scanned conditional controls and record exact lexical ownership.
///
/// Malformed control sequences are diagnosed, but their individual intervals
/// remain in the census and therefore stay protected from generic edits.
static std::vector<ScannedConditionalGroup>
buildConditionalTopology(std::vector<ScannedDirective> &directives,
                         std::vector<std::string> &diagnostics) {
  std::vector<OpenConditionalGroup> stack;
  std::vector<ScannedConditionalGroup> groups;

  for (size_t directiveIndex = 0; directiveIndex < directives.size();
       ++directiveIndex) {
    ScannedDirective &directive = directives[directiveIndex];
    const PreprocessingStructureKind kind = directive.interval.kind;

    // A producer-backed pragma operator can be nested physically inside a
    // conditional-control line.  Such an operator is owned by the control
    // line's outer arm, not by the arm that the control line opens or selects.
    // Copy that already-computed lexical owner before applying the ordinary
    // stack rule below.
    bool nestedInConditionalControl = false;
    for (size_t priorIndex = directiveIndex; priorIndex > 0; --priorIndex) {
      const ScannedDirective &prior = directives[priorIndex - 1];
      if (prior.interval.begin > directive.interval.begin)
        continue;
      if (prior.interval.end <= directive.interval.begin)
        continue;
      if (!isConditionalControl(prior.interval.kind) ||
          directive.interval.end > prior.interval.end)
        continue;
      directive.lexicalOwnerArmIndex = prior.lexicalOwnerArmIndex;
      nestedInConditionalControl = true;
      break;
    }

    // A group's own #elif/#else/#endif lines are outside its arm bodies.  Their
    // lexical owner is therefore the current arm of the containing outer group,
    // not the arm that precedes the control line in this group.
    if (!nestedInConditionalControl &&
        (isConditionalMiddle(kind) ||
         kind == PreprocessingStructureKind::ConditionalEndif)) {
      if (stack.size() > 1)
        directive.lexicalOwnerArmIndex =
            stack[stack.size() - 2].currentArmDirectiveIndex;
    } else if (!nestedInConditionalControl && !stack.empty()) {
      directive.lexicalOwnerArmIndex = stack.back().currentArmDirectiveIndex;
    }

    if (isConditionalOpening(kind)) {
      OpenConditionalGroup open;
      open.openingDirectiveIndex = directiveIndex;
      open.armDirectiveIndices.push_back(directiveIndex);
      open.currentArmDirectiveIndex = directiveIndex;
      stack.push_back(std::move(open));
      continue;
    }

    if (isConditionalMiddle(kind)) {
      if (stack.empty()) {
        diagnostics.push_back(
            llvm::formatv("conditional control {0} at byte {1} has no open "
                          "conditional group",
                          toString(kind), directive.interval.begin)
                .str());
        continue;
      }
      stack.back().armDirectiveIndices.push_back(directiveIndex);
      stack.back().currentArmDirectiveIndex = directiveIndex;
      continue;
    }

    if (kind != PreprocessingStructureKind::ConditionalEndif)
      continue;
    if (stack.empty()) {
      diagnostics.push_back(
          llvm::formatv("#endif at byte {0} has no open conditional group",
                        directive.interval.begin)
              .str());
      continue;
    }

    OpenConditionalGroup open = std::move(stack.back());
    stack.pop_back();
    groups.push_back(ScannedConditionalGroup{
        open.openingDirectiveIndex, std::move(open.armDirectiveIndices),
        directiveIndex, directives[open.openingDirectiveIndex].interval.begin,
        directive.interval.end});
  }

  for (const OpenConditionalGroup &open : stack) {
    diagnostics.push_back(
        llvm::formatv("conditional group opened at byte {0} has no #endif",
                      directives[open.openingDirectiveIndex].interval.begin)
            .str());
  }

  llvm::sort(groups, scannedConditionalGroupLess);
  return groups;
}

/// Return whether a producer conditional-group start names the exact lexical
/// opening control line.
///
/// Current producer maps record `group_b` at the beginning of the physical
/// logical-line prefix, so leading preprocessing trivia before `#` is included.
/// The published schema historically described the same field as the exact
/// directive-introducer byte.  Both coordinates are canonical boundaries
/// already recovered by the shared scanner.  Accepting either preserves exact
/// producer binding across both map conventions without permitting nearest-byte
/// matching or any offset search through the leading trivia.
static bool producerGroupBeginMatchesOpeningDirective(
    const RefoldModel::CondGroup &producerGroup,
    const ScannedDirective &openingDirective) {
  return producerGroup.groupB == openingDirective.interval.begin ||
         producerGroup.groupB == openingDirective.introducerBegin;
}

/// Return the producer schema spelling corresponding to one arm directive.
static StringRef producerArmKind(PreprocessingStructureKind kind) {
  switch (kind) {
  case PreprocessingStructureKind::ConditionalIf:
    return "if";
  case PreprocessingStructureKind::ConditionalIfdef:
    return "ifdef";
  case PreprocessingStructureKind::ConditionalIfndef:
    return "ifndef";
  case PreprocessingStructureKind::ConditionalElif:
    return "elif";
  case PreprocessingStructureKind::ConditionalElifdef:
    return "elifdef";
  case PreprocessingStructureKind::ConditionalElifndef:
    return "elifndef";
  case PreprocessingStructureKind::ConditionalElse:
    return "else";
  default:
    return StringRef();
  }
}

/// Attach exact producer conditional group/arm identities to lexical controls.
static void bindConditionalGroups(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
    std::vector<ScannedDirective> &directives,
    ArrayRef<ScannedConditionalGroup> scannedGroups,
    std::vector<std::string> &diagnostics) {
  std::vector<uint64_t> matchedProducerGroupIds;

  for (const ScannedConditionalGroup &scannedGroup : scannedGroups) {
    const ScannedDirective &openingDirective =
        directives[scannedGroup.openingDirectiveIndex];
    const PreprocessingStructureInterval &opening = openingDirective.interval;
    const PreprocessingStructureInterval &ending =
        directives[scannedGroup.endifDirectiveIndex].interval;

    // Bind nested groups only after their lexical outer arm has itself acquired
    // one exact producer id.  This proves the complete producer parent chain
    // instead of accepting a coincident byte range in the wrong conditional
    // topology.
    std::optional<uint64_t> lexicalParentArmId;
    if (openingDirective.lexicalOwnerArmIndex) {
      const size_t parentArmIndex = *openingDirective.lexicalOwnerArmIndex;
      if (parentArmIndex >= directives.size() ||
          !directives[parentArmIndex].interval.conditionalArmId) {
        diagnostics.push_back(
            llvm::formatv("conditional group at range=[{0},{1}) has no exact "
                          "lexical parent-arm binding",
                          opening.begin, ending.end)
                .str());
        continue;
      }
      lexicalParentArmId =
          directives[parentArmIndex].interval.conditionalArmId;
    }

    // The current producer schema does not serialize an independent record for
    // every #elif/#else/#endif control. Recover those identities only through
    // exact redundant boundaries: group_b must name either canonical opening
    // boundary accepted by `producerGroupBeginMatchesOpeningDirective()`,
    // group_e must equal the complete #endif logical-line end, and every arm's
    // body_b must equal its scanned control-line end. No nearest directive or
    // source-order fallback is permitted when any boundary disagrees.
    std::vector<const RefoldModel::CondGroup *> candidates;
    for (const RefoldModel::CondGroup &producerGroup : model.GetConds()) {
      if (!paths.PathsEqual(producerGroup.file, sourcePath) ||
          producerGroup.parentIncludeId != ownerIncludeId ||
          producerGroup.parentArmId != lexicalParentArmId)
        continue;
      if (producerGroupBeginMatchesOpeningDirective(
              producerGroup, openingDirective) &&
          producerGroup.groupE == ending.end) {
        candidates.push_back(&producerGroup);
      }
    }

    if (candidates.empty()) {
      diagnostics.push_back(
          llvm::formatv("lexical conditional group line=[{0},{1}) "
                        "introducer={2} has no exact producer binding in owner "
                        "domain {3}",
                        openingDirective.interval.begin, ending.end,
                        openingDirective.introducerBegin,
                        ownerDomainDescription(ownerIncludeId))
              .str());
      continue;
    }
    if (candidates.size() != 1) {
      diagnostics.push_back(
          llvm::formatv("lexical conditional group line=[{0},{1}) "
                        "introducer={2} has {3} producer bindings; exact "
                        "ownership is ambiguous",
                        openingDirective.interval.begin, ending.end,
                        openingDirective.introducerBegin, candidates.size())
              .str());
      continue;
    }

    const RefoldModel::CondGroup &producerGroup = *candidates.front();
    if (llvm::is_contained(matchedProducerGroupIds, producerGroup.id)) {
      diagnostics.push_back(
          llvm::formatv("conditional group id={0} was selected by more than "
                        "one lexical source group",
                        producerGroup.id)
              .str());
      continue;
    }

    if (producerGroup.arms.size() != scannedGroup.armDirectiveIndices.size()) {
      diagnostics.push_back(
          llvm::formatv(
              "conditional group id={0} arm count mismatch: model={1} "
              "source={2}",
              producerGroup.id, producerGroup.arms.size(),
              scannedGroup.armDirectiveIndices.size())
              .str());
      continue;
    }

    bool exactArms = true;
    for (size_t armIndex = 0; armIndex < producerGroup.arms.size();
         ++armIndex) {
      const RefoldModel::CondArm &producerArm = producerGroup.arms[armIndex];
      const ScannedDirective &scannedArm =
          directives[scannedGroup.armDirectiveIndices[armIndex]];
      if (producerArm.kind != producerArmKind(scannedArm.interval.kind) ||
          producerArm.bodyB != scannedArm.interval.end) {
        exactArms = false;
        break;
      }
    }
    if (!exactArms) {
      diagnostics.push_back(
          llvm::formatv("conditional group id={0} could not be bound to exact "
                        "arm-control directive intervals",
                        producerGroup.id)
              .str());
      continue;
    }

    for (size_t armIndex = 0; armIndex < producerGroup.arms.size();
         ++armIndex) {
      ScannedDirective &scannedArm =
          directives[scannedGroup.armDirectiveIndices[armIndex]];
      scannedArm.interval.modelKind =
          PreprocessingStructureModelKind::ConditionalDirective;
      scannedArm.interval.conditionalGroupId = producerGroup.id;
      scannedArm.interval.conditionalArmId = producerGroup.arms[armIndex].id;
    }

    ScannedDirective &scannedEndif =
        directives[scannedGroup.endifDirectiveIndex];
    scannedEndif.interval.modelKind =
        PreprocessingStructureModelKind::ConditionalDirective;
    scannedEndif.interval.conditionalGroupId = producerGroup.id;
    matchedProducerGroupIds.push_back(producerGroup.id);
  }

  // A producer group in this exact source-owner domain must have one complete
  // lexical #if...#endif match.  Otherwise later state-aware reconstruction
  // cannot use the index as producer authority, even though each individually
  // scanned directive remains protected from generic edits.
  for (const RefoldModel::CondGroup &producerGroup : model.GetConds()) {
    if (!paths.PathsEqual(producerGroup.file, sourcePath) ||
        producerGroup.parentIncludeId != ownerIncludeId)
      continue;
    if (llvm::is_contained(matchedProducerGroupIds, producerGroup.id))
      continue;
    diagnostics.push_back(
        llvm::formatv("producer conditional group id={0} range=[{1},{2}) has "
                      "no unique exact lexical binding",
                      producerGroup.id, producerGroup.groupB,
                      producerGroup.groupE)
            .str());
  }

  // Every lexical conditional control must be producer-bound before the index
  // can authorize conditional-state reconstruction.  The intervals remain in
  // the protected census when a binding is absent, but the diagnostic makes
  // IsComplete() false so all producer-authoritative consumers fail closed.
  for (const ScannedDirective &directive : directives) {
    if (!isConditionalControl(directive.interval.kind) ||
        directive.interval.IsProducerBound())
      continue;
    diagnostics.push_back(
        llvm::formatv("conditional control {0} range=[{1},{2}) has no unique "
                      "exact producer group/arm binding",
                      toString(directive.interval.kind),
                      directive.interval.begin, directive.interval.end)
            .str());
  }

  // Resolve lexical containing-arm links only after every exact group binding
  // has assigned producer arm ids.
  for (ScannedDirective &directive : directives) {
    if (!directive.lexicalOwnerArmIndex)
      continue;
    const size_t ownerIndex = *directive.lexicalOwnerArmIndex;
    if (ownerIndex >= directives.size())
      continue;
    directive.interval.ownerConditionalArmId =
        directives[ownerIndex].interval.conditionalArmId;
  }
}

/// Select the unique scanned directive containing one producer text range.
static std::optional<size_t> findUniqueDirectiveForProducerRange(
    ArrayRef<ScannedDirective> directives, uint64_t begin, uint64_t end,
    PreprocessingStructureKind kind) {
  std::optional<size_t> found;
  for (size_t index = 0; index < directives.size(); ++index) {
    const ScannedDirective &directive = directives[index];
    if (!isDirectiveKind(directive.interval.kind, kind))
      continue;

    // Producer directive text may include leading horizontal trivia before the
    // introducer or omit the terminating newline.  The lexical interval is the
    // authoritative complete logical line; the producer range is accepted only
    // when it encloses the introducer and is itself fully contained in that
    // line.  A producer spelling may not widen protection into the preceding
    // logical line even when its bytes happen to match.
    if (begin < directive.interval.begin ||
        begin > directive.introducerBegin ||
        directive.introducerBegin >= end || end > directive.interval.end)
      continue;

    if (found)
      return std::nullopt;
    found = index;
  }
  return found;
}

/// Attach one producer identity, rejecting cross-array or duplicate ambiguity.
static void attachModelBinding(ScannedDirective &directive,
                               PreprocessingStructureModelKind modelKind,
                               uint64_t modelItemId, uint64_t producerTextBegin,
                               uint64_t producerTextEnd,
                               std::vector<std::string> &diagnostics) {
  if (directive.modelBindingAmbiguous)
    return;
  const bool producerRangeValid =
      directive.interval.begin <= producerTextBegin &&
      producerTextBegin < producerTextEnd &&
      producerTextEnd <= directive.interval.end;
  if (!producerRangeValid) {
    diagnostics.push_back(
        llvm::formatv("preprocessing interval [{0},{1}) received invalid "
                      "producer text range [{2},{3})",
                      directive.interval.begin, directive.interval.end,
                      producerTextBegin, producerTextEnd)
            .str());
    directive.modelBindingAmbiguous = true;
    return;
  }

  if (directive.interval.modelKind == PreprocessingStructureModelKind::None &&
      !directive.interval.modelItemId) {
    directive.interval.modelKind = modelKind;
    directive.interval.modelItemId = modelItemId;
    directive.interval.producerTextBegin = producerTextBegin;
    directive.interval.producerTextEnd = producerTextEnd;
    return;
  }
  if (directive.interval.modelKind == modelKind &&
      directive.interval.modelItemId == modelItemId &&
      directive.interval.producerTextBegin == producerTextBegin &&
      directive.interval.producerTextEnd == producerTextEnd)
    return;

  diagnostics.push_back(
      llvm::formatv("preprocessing interval [{0},{1}) has ambiguous producer "
                    "bindings ({2}:{3} and {4}:{5})",
                    directive.interval.begin, directive.interval.end,
                    toString(directive.interval.modelKind),
                    directive.interval.modelItemId.value_or(
                        std::numeric_limits<uint64_t>::max()),
                    toString(modelKind), modelItemId)
          .str());
  directive.interval.modelKind = PreprocessingStructureModelKind::None;
  directive.interval.modelItemId.reset();
  directive.interval.producerTextBegin.reset();
  directive.interval.producerTextEnd.reset();
  directive.modelBindingAmbiguous = true;
}

/// Bind #define/#undef records through the shared full-line recovery theorem.
static void bindMacroDirectives(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    const RefoldMacroStateProof &macroStateProof, StringRef sourcePath,
    StringRef sourceBytes, std::optional<uint64_t> ownerIncludeId,
    std::vector<ScannedDirective> &directives,
    std::vector<std::string> &diagnostics) {
  for (const RefoldModel::MacroDirective &modelDirective :
       model.GetMacroDirectives()) {
    if (!paths.PathsEqual(modelDirective.sitePath, sourcePath) ||
        !ownerMatches(modelDirective.ownerIncludeId, ownerIncludeId))
      continue;

    std::optional<MacroStateDirectiveLineInterval> recovered =
        macroStateProof.RecoverMacroStateDirectiveLineInterval(
            modelDirective, sourcePath, sourceBytes, ownerIncludeId);
    if (!recovered) {
      diagnostics.push_back(
          llvm::formatv("macro directive id={0} has no exact full-line source "
                        "interval",
                        modelDirective.id)
              .str());
      continue;
    }

    const PreprocessingStructureKind expectedKind =
        modelDirective.subkind == "#define"
            ? PreprocessingStructureKind::MacroDefine
            : PreprocessingStructureKind::MacroUndef;
    std::optional<size_t> scannedIndex = findUniqueDirectiveForProducerRange(
        directives, recovered->begin, recovered->end, expectedKind);
    if (!scannedIndex) {
      diagnostics.push_back(
          llvm::formatv("macro directive id={0} recovered range=[{1},{2}) "
                        "does not select one lexical {3} directive",
                        modelDirective.id, recovered->begin, recovered->end,
                        toString(expectedKind))
              .str());
      continue;
    }

    ScannedDirective &scanned = directives[*scannedIndex];
    attachModelBinding(scanned,
                       PreprocessingStructureModelKind::MacroDirective,
                       modelDirective.id, recovered->begin, recovered->end,
                       diagnostics);
  }
}

/// Return the protected lexical class for one producer include record.
static PreprocessingStructureKind
includeStructureKind(const RefoldModel::IncludeItem &include) {
  if (include.subkind == "#include_next")
    return PreprocessingStructureKind::IncludeNext;
  return PreprocessingStructureKind::Include;
}

/// Bind exact #include/#include_next producer lines in one owner domain.
static void bindIncludeDirectives(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    StringRef sourcePath, StringRef sourceBytes,
    std::optional<uint64_t> ownerIncludeId,
    std::vector<ScannedDirective> &directives,
    std::vector<std::string> &diagnostics) {
  for (const RefoldModel::IncludeItem &include : model.GetIncludes()) {
    if (!paths.PathsEqual(include.sitePath, sourcePath) ||
        !ownerMatches(include.parent, ownerIncludeId))
      continue;

    std::optional<ExactProducerTextRange> recovered =
        recoverExactProducerTextRange(sourceBytes, include.siteB,
                                      include.siteE, include.text);
    if (!recovered) {
      diagnostics.push_back(
          llvm::formatv("include directive id={0} source range=[{1},{2}) does "
                        "not recover its producer text exactly",
                        include.id, include.siteB, include.siteE)
              .str());
      continue;
    }

    const PreprocessingStructureKind expectedKind =
        includeStructureKind(include);
    std::optional<size_t> scannedIndex = findUniqueDirectiveForProducerRange(
        directives, recovered->begin, recovered->end, expectedKind);
    if (!scannedIndex) {
      diagnostics.push_back(
          llvm::formatv("include directive id={0} recovered range=[{1},{2}) "
                        "does not select one lexical {3} directive",
                        include.id, recovered->begin, recovered->end,
                        toString(expectedKind))
              .str());
      continue;
    }

    ScannedDirective &scanned = directives[*scannedIndex];
    attachModelBinding(scanned,
                       PreprocessingStructureModelKind::IncludeDirective,
                       include.id, recovered->begin, recovered->end,
                       diagnostics);
  }
}

/// Bind exact #line or GNU line-marker records in one owner domain.
static void bindLineControlDirectives(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    StringRef sourcePath, StringRef sourceBytes,
    std::optional<uint64_t> ownerIncludeId,
    std::vector<ScannedDirective> &directives,
    std::vector<std::string> &diagnostics) {
  for (const RefoldModel::LineControlEvent &event : model.GetLineControls()) {
    if (!paths.PathsEqual(event.physicalFile, sourcePath) ||
        !ownerMatches(event.ownerIncludeId, ownerIncludeId))
      continue;
    if (!event.siteB || !event.siteE) {
      diagnostics.push_back(
          llvm::formatv("line-control event id={0} has no physical source "
                        "range in this owner",
                        event.id)
              .str());
      continue;
    }
    std::optional<ExactProducerTextRange> recovered =
        recoverExactProducerTextRange(sourceBytes, *event.siteB,
                                      *event.siteE, event.text);
    if (!recovered) {
      diagnostics.push_back(
          llvm::formatv("line-control event id={0} source range=[{1},{2}) "
                        "does not recover its producer text exactly",
                        event.id, *event.siteB, *event.siteE)
              .str());
      continue;
    }

    std::optional<size_t> scannedIndex = findUniqueDirectiveForProducerRange(
        directives, recovered->begin, recovered->end,
        PreprocessingStructureKind::LineControl);
    if (!scannedIndex) {
      diagnostics.push_back(
          llvm::formatv("line-control event id={0} recovered range=[{1},{2}) "
                        "does not select one lexical line-control directive",
                        event.id, recovered->begin, recovered->end)
              .str());
      continue;
    }

    ScannedDirective &scanned = directives[*scannedIndex];
    attachModelBinding(scanned,
                       PreprocessingStructureModelKind::LineControlEvent,
                       event.id, recovered->begin, recovered->end,
                       diagnostics);
  }
}

/// Return the unique already-inventoried pragma-operator interval for an exact
/// producer range.
///
/// Several include occurrences may produce distinct pragma records for the same
/// physical bytes.  They must share one protected source interval; owner-local
/// model binding is attached separately and rejects conflicting records.
static std::optional<size_t> findPragmaOperatorInterval(
    ArrayRef<ScannedDirective> directives, uint64_t begin, uint64_t end,
    bool &ambiguous) {
  ambiguous = false;
  std::optional<size_t> found;
  for (size_t index = 0; index < directives.size(); ++index) {
    const ScannedDirective &directive = directives[index];
    if (directive.interval.kind !=
            PreprocessingStructureKind::PragmaOperator ||
        directive.interval.begin != begin || directive.interval.end != end)
      continue;
    if (found) {
      ambiguous = true;
      return std::nullopt;
    }
    found = index;
  }
  return found;
}

/// Add producer-proven `_Pragma` operator ranges before conditional topology is
/// calculated.
///
/// Raw-token scanning discovers directly spelled operators.  The producer's
/// dedicated `operator_b/e` coordinates additionally represent occurrences
/// whose effective operator came through macro expansion and therefore need
/// not have one lexically recoverable `_Pragma` spelling at the event site.  A
/// valid range is protected in every concrete occurrence of the physical
/// source file; an exact producer binding is attached only in the matching
/// include-owner domain.  This prevents a repeated-header record from becoming
/// authority for another include instance while still keeping the shared bytes
/// protected.
static void appendPragmaOperatorIntervals(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    StringRef sourcePath, StringRef sourceBytes,
    std::optional<uint64_t> ownerIncludeId,
    std::vector<ScannedDirective> &directives,
    std::vector<std::string> &diagnostics) {
  for (const RefoldModel::PragmaDirective &pragma : model.GetPragmas()) {
    if (!pragma.viaPragmaOperator ||
        !paths.PathsEqual(pragma.sitePath, sourcePath))
      continue;

    if (!pragma.operatorB || !pragma.operatorE ||
        !sourceRangeValid(sourceBytes, *pragma.operatorB, *pragma.operatorE)) {
      if (ownerMatches(pragma.ownerIncludeId, ownerIncludeId)) {
        diagnostics.push_back(
            llvm::formatv("_Pragma record id={0} has no valid exact operator "
                          "range in this source owner",
                          pragma.id)
                .str());
      }
      continue;
    }

    bool ambiguousRange = false;
    std::optional<size_t> existing = findPragmaOperatorInterval(
        directives, *pragma.operatorB, *pragma.operatorE, ambiguousRange);
    if (ambiguousRange) {
      if (ownerMatches(pragma.ownerIncludeId, ownerIncludeId)) {
        diagnostics.push_back(
            llvm::formatv("_Pragma record id={0} range=[{1},{2}) selects "
                          "multiple protected operator intervals",
                          pragma.id, *pragma.operatorB, *pragma.operatorE)
                .str());
      }
      continue;
    }
    if (!existing) {
      ScannedDirective operatorInterval;
      operatorInterval.interval.kind =
          PreprocessingStructureKind::PragmaOperator;
      operatorInterval.interval.sourcePath = sourcePath.str();
      operatorInterval.interval.ownerIncludeId = ownerIncludeId;
      operatorInterval.interval.begin = *pragma.operatorB;
      operatorInterval.interval.end = *pragma.operatorE;
      operatorInterval.interval.structureSpellingBegin = *pragma.operatorB;
      operatorInterval.interval.structureSpellingEnd = *pragma.operatorE;
      operatorInterval.introducerBegin = *pragma.operatorB;
      directives.push_back(std::move(operatorInterval));
      existing = directives.size() - 1;
    }

    if (!ownerMatches(pragma.ownerIncludeId, ownerIncludeId))
      continue;
    attachModelBinding(directives[*existing],
                       PreprocessingStructureModelKind::PragmaDirective,
                       pragma.id, *pragma.operatorB, *pragma.operatorE,
                       diagnostics);
  }
}

/// Bind ordinary `#pragma` directive lines to exact producer records.
///
/// The complete lexical logical-line interval remains authoritative for source
/// protection.  Producer text may start before `#` or omit the terminating
/// newline, but it must match its own range byte-for-byte and select exactly
/// one scanned `#pragma` line before the record is attached.
static void bindPragmaDirectives(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    StringRef sourcePath, StringRef sourceBytes,
    std::optional<uint64_t> ownerIncludeId,
    std::vector<ScannedDirective> &directives,
    std::vector<std::string> &diagnostics) {
  for (const RefoldModel::PragmaDirective &pragma : model.GetPragmas()) {
    if (pragma.viaPragmaOperator ||
        !paths.PathsEqual(pragma.sitePath, sourcePath) ||
        !ownerMatches(pragma.ownerIncludeId, ownerIncludeId))
      continue;

    std::optional<ExactProducerTextRange> recovered =
        recoverExactProducerTextRange(sourceBytes, pragma.siteB,
                                      pragma.siteE, pragma.text);
    if (!recovered) {
      diagnostics.push_back(
          llvm::formatv("pragma directive id={0} source range=[{1},{2}) does "
                        "not recover its producer text exactly",
                        pragma.id, pragma.siteB, pragma.siteE)
              .str());
      continue;
    }

    std::optional<size_t> scannedIndex = findUniqueDirectiveForProducerRange(
        directives, recovered->begin, recovered->end,
        PreprocessingStructureKind::Pragma);
    if (!scannedIndex) {
      diagnostics.push_back(
          llvm::formatv("pragma directive id={0} recovered range=[{1},{2}) "
                        "does not select one lexical #pragma directive",
                        pragma.id, recovered->begin, recovered->end)
              .str());
      continue;
    }

    ScannedDirective &scanned = directives[*scannedIndex];
    attachModelBinding(scanned,
                       PreprocessingStructureModelKind::PragmaDirective,
                       pragma.id, recovered->begin, recovered->end,
                       diagnostics);
  }
}

/// Deterministic final ordering for immutable public intervals.
static bool intervalLess(const PreprocessingStructureInterval &lhs,
                         const PreprocessingStructureInterval &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  if (lhs.end != rhs.end)
    return lhs.end < rhs.end;
  if (lhs.kind != rhs.kind)
    return static_cast<unsigned>(lhs.kind) < static_cast<unsigned>(rhs.kind);
  if (lhs.ownerIncludeId != rhs.ownerIncludeId)
    return lhs.ownerIncludeId < rhs.ownerIncludeId;
  if (lhs.conditionalGroupId != rhs.conditionalGroupId)
    return lhs.conditionalGroupId < rhs.conditionalGroupId;
  if (lhs.conditionalArmId != rhs.conditionalArmId)
    return lhs.conditionalArmId < rhs.conditionalArmId;
  if (lhs.structureSpellingBegin != rhs.structureSpellingBegin)
    return lhs.structureSpellingBegin < rhs.structureSpellingBegin;
  if (lhs.structureSpellingEnd != rhs.structureSpellingEnd)
    return lhs.structureSpellingEnd < rhs.structureSpellingEnd;
  if (lhs.modelKind != rhs.modelKind)
    return static_cast<unsigned>(lhs.modelKind) <
           static_cast<unsigned>(rhs.modelKind);
  if (lhs.modelItemId != rhs.modelItemId)
    return lhs.modelItemId < rhs.modelItemId;
  if (lhs.producerTextBegin != rhs.producerTextBegin)
    return lhs.producerTextBegin < rhs.producerTextBegin;
  return lhs.producerTextEnd < rhs.producerTextEnd;
}

} // namespace

RefoldPreprocessingStructureIndex RefoldPreprocessingStructureIndex::Build(
    Dependencies deps, StringRef sourcePath, StringRef sourceBytes,
    std::optional<uint64_t> ownerIncludeId) {
  RefoldPreprocessingStructureIndex index;
  index.sourcePath_ = sourcePath.str();
  index.sourceSize_ = static_cast<uint64_t>(sourceBytes.size());
  index.ownerIncludeId_ = ownerIncludeId;

  // Keep local producer-record binding separate from failures that make the
  // physical protection inventory itself incomplete.  A normal binding
  // mismatch does not erase the lexically discovered directive interval, so a
  // direct span can still reject that interval locally.  Scanner/topology
  // failures, non-unique conditional bindings, and unlocatable
  // expansion-derived pragma operators cannot be localized and therefore
  // reject every direct TU byte proof.
  std::vector<std::string> protectionDiagnostics;
  std::vector<ScannedDirective> directives = scanPreprocessingStructure(
      sourcePath, sourceBytes, ownerIncludeId, deps.lexLang,
      index.lexicalTokenIntervals_, index.triviaIntervals_,
      index.indivisibleTriviaIntervals_, protectionDiagnostics);

  // Supplement directly scanned `_Pragma` expressions with producer-proven
  // expansion-derived occurrences before source ordering and conditional
  // topology.  Both forms then receive the same exact enclosing-arm ownership
  // as ordinary directive lines.
  appendPragmaOperatorIntervals(deps.model, deps.paths, sourcePath, sourceBytes,
                                ownerIncludeId, directives,
                                protectionDiagnostics);
  llvm::sort(directives, scannedDirectiveLess);

  std::vector<ScannedConditionalGroup> conditionalGroups =
      buildConditionalTopology(directives, protectionDiagnostics);

  // Conditional topology is bound before ordinary directives so every later
  // interval can inherit the exact outer conditional-arm owner.
  bindConditionalGroups(deps.model, deps.paths, sourcePath, ownerIncludeId,
                        directives, conditionalGroups, protectionDiagnostics);
  bindMacroDirectives(deps.model, deps.paths, deps.macroStateProof, sourcePath,
                      sourceBytes, ownerIncludeId, directives,
                      index.diagnostics_);
  bindIncludeDirectives(deps.model, deps.paths, sourcePath, sourceBytes,
                        ownerIncludeId, directives, index.diagnostics_);
  bindLineControlDirectives(deps.model, deps.paths, sourcePath, sourceBytes,
                            ownerIncludeId, directives, index.diagnostics_);
  bindPragmaDirectives(deps.model, deps.paths, sourcePath, sourceBytes,
                       ownerIncludeId, directives, index.diagnostics_);

  index.intervals_.reserve(directives.size());
  for (ScannedDirective &directive : directives) {
    if (!directive.interval.IsValid()) {
      protectionDiagnostics.push_back(
          llvm::formatv("discarded invalid preprocessing interval [{0},{1})",
                        directive.interval.begin, directive.interval.end)
              .str());
      continue;
    }
    index.intervals_.push_back(std::move(directive.interval));
  }
  llvm::sort(index.intervals_, intervalLess);

  index.directTUProtectionDiagnostics_ = protectionDiagnostics;
  index.diagnostics_.insert(index.diagnostics_.end(),
                            protectionDiagnostics.begin(),
                            protectionDiagnostics.end());

  // Direct TU planning queries this index for every candidate token span.
  // Retain a prefix maximum of interval ends so overlap lookup can skip every
  // source-ordered prefix that is proven to end before the queried range, even
  // when protected intervals overlap one another.
  index.prefixMaximumIntervalEnds_.reserve(index.intervals_.size());
  uint64_t prefixMaximumEnd = 0;
  for (const PreprocessingStructureInterval &interval : index.intervals_) {
    prefixMaximumEnd = std::max(prefixMaximumEnd, interval.end);
    index.prefixMaximumIntervalEnds_.push_back(prefixMaximumEnd);
  }
  return index;
}

bool RefoldPreprocessingStructureIndex::HasUnboundStructure() const {
  for (const PreprocessingStructureInterval &interval : intervals_) {
    if (!interval.IsProducerBound())
      return true;
  }
  return false;
}

std::vector<const PreprocessingStructureInterval *>
RefoldPreprocessingStructureIndex::FindOverlapping(uint64_t begin,
                                                   uint64_t end) const {
  std::vector<const PreprocessingStructureInterval *> result;
  if (end <= begin)
    return result;

  auto firstPossible = std::upper_bound(prefixMaximumIntervalEnds_.begin(),
                                        prefixMaximumIntervalEnds_.end(),
                                        begin);
  size_t intervalIndex = static_cast<size_t>(
      std::distance(prefixMaximumIntervalEnds_.begin(), firstPossible));
  for (; intervalIndex < intervals_.size(); ++intervalIndex) {
    const PreprocessingStructureInterval &interval =
        intervals_[intervalIndex];
    if (interval.begin >= end)
      break;
    if (interval.Overlaps(begin, end))
      result.push_back(&interval);
  }
  return result;
}

bool RefoldPreprocessingStructureIndex::IsRangeLexicallyIgnorable(
    uint64_t begin, uint64_t end) const {
  if (end < begin)
    return false;
  if (begin == end)
    return true;

  auto triviaIt = std::lower_bound(
      triviaIntervals_.begin(), triviaIntervals_.end(), begin,
      [](const PreprocessingTriviaInterval &interval, uint64_t offset) {
        return interval.end <= offset;
      });
  bool containedByTrivia = triviaIt != triviaIntervals_.end() &&
                           triviaIt->Contains(begin, end);
  if (!containedByTrivia)
    return false;

  // A complete comment or escaped-newline spelling is trivia only as a whole.
  // Reject a range boundary inside either component; otherwise a corrupt token
  // map could make a partial comment or backslash-newline sequence look like
  // independently removable whitespace.
  return IsExactLexicalBoundary(begin) && IsExactLexicalBoundary(end);
}

bool RefoldPreprocessingStructureIndex::IsExactTokenSpellingInterval(
    uint64_t begin, uint64_t end) const {
  if (end <= begin)
    return false;

  auto tokenIt = std::lower_bound(
      lexicalTokenIntervals_.begin(), lexicalTokenIntervals_.end(), begin,
      [](const PreprocessingLexicalTokenInterval &interval, uint64_t offset) {
        return interval.begin < offset;
      });
  return tokenIt != lexicalTokenIntervals_.end() &&
         tokenIt->Equals(begin, end);
}

bool RefoldPreprocessingStructureIndex::IsExactLexicalBoundary(
    uint64_t offset) const {
  auto tokenIt = std::upper_bound(
      lexicalTokenIntervals_.begin(), lexicalTokenIntervals_.end(), offset,
      [](uint64_t boundary,
         const PreprocessingLexicalTokenInterval &interval) {
        return boundary < interval.begin;
      });
  if (tokenIt != lexicalTokenIntervals_.begin()) {
    --tokenIt;
    if (tokenIt->ContainsInteriorBoundary(offset))
      return false;
  }

  auto componentIt = std::upper_bound(
      indivisibleTriviaIntervals_.begin(), indivisibleTriviaIntervals_.end(),
      offset,
      [](uint64_t boundary,
         const PreprocessingIndivisibleTriviaInterval &interval) {
        return boundary < interval.begin;
      });
  if (componentIt == indivisibleTriviaIntervals_.begin())
    return true;
  --componentIt;
  return !componentIt->ContainsInteriorBoundary(offset);
}


namespace {

/// Return whether one protected interval is exact producer-bound macro-state
/// evidence for a specialized repair theorem.
static bool isExactProducerBoundMacroStateInterval(
    const PreprocessingStructureInterval &interval) {
  if (interval.kind != PreprocessingStructureKind::MacroDefine &&
      interval.kind != PreprocessingStructureKind::MacroUndef) {
    return false;
  }
  return interval.IsValid() &&
         interval.modelKind ==
             PreprocessingStructureModelKind::MacroDirective &&
         interval.IsProducerBound();
}

} // namespace

bool RefoldPreprocessingStructureIndex::ProveOrdinaryDirectTUInternalGap(
    uint64_t begin, uint64_t end) const {
  if (end < begin || end > sourceSize_ ||
      !IsDirectTUProtectionCensusComplete() ||
      !IsExactLexicalBoundary(begin) || !IsExactLexicalBoundary(end)) {
    return false;
  }
  if (begin == end)
    return true;

  // Lexer trivia cannot become ordinary edit authority merely because the same
  // physical bytes also belong to a directive logical line.  Consult the exact
  // structure census independently before accepting the trivia theorem.
  if (!FindOverlapping(begin, end).empty())
    return false;

  return IsRangeLexicallyIgnorable(begin, end);
}

bool RefoldPreprocessingStructureIndex::CollectExactMacroStateIntervals(
    uint64_t begin, uint64_t end,
    std::vector<const PreprocessingStructureInterval *> &intervals) const {
  intervals.clear();
  if (end < begin || end > sourceSize_ ||
      !IsDirectTUProtectionCensusComplete() ||
      !IsExactLexicalBoundary(begin) || !IsExactLexicalBoundary(end)) {
    return false;
  }

  for (const PreprocessingStructureInterval *interval :
       FindOverlapping(begin, end)) {
    if (!isExactProducerBoundMacroStateInterval(*interval))
      continue;
    if (interval->begin < begin || end < interval->end) {
      intervals.clear();
      return false;
    }
    intervals.push_back(interval);
  }
  return true;
}

} // namespace refold
} // namespace clang

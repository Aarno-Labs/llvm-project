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

#include "model/RefoldModel.h"
#include "model/RefoldPathIdentity.h"
#include "support/StringUtils.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
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
  bool modelBindingAmbiguous = false;
};

/// Return whether a producer record belongs to the concrete indexed owner.
static bool ownerMatches(std::optional<uint64_t> recordOwner,
                         std::optional<uint64_t> requestedOwner) {
  return recordOwner == requestedOwner;
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

/// Return whether `kind` participates in conditional-control topology.
static bool isConditionalControl(PreprocessingStructureKind kind) {
  switch (kind) {
  case PreprocessingStructureKind::ConditionalIf:
  case PreprocessingStructureKind::ConditionalIfdef:
  case PreprocessingStructureKind::ConditionalIfndef:
  case PreprocessingStructureKind::ConditionalElif:
  case PreprocessingStructureKind::ConditionalElifdef:
  case PreprocessingStructureKind::ConditionalElifndef:
  case PreprocessingStructureKind::ConditionalElse:
  case PreprocessingStructureKind::ConditionalEndif:
    return true;
  default:
    return false;
  }
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
                           std::vector<PreprocessingTriviaInterval>
                               &commentIntervals,
                           std::vector<std::string> &diagnostics) {
  PreprocessingDirectiveScanResult lexicalScan =
      scanPreprocessingDirectives(sourceBytes, lexLang);
  lexicalTokenIntervals = std::move(lexicalScan.lexicalTokenIntervals);
  triviaIntervals = std::move(lexicalScan.triviaIntervals);
  indivisibleTriviaIntervals =
      std::move(lexicalScan.indivisibleTriviaIntervals);
  commentIntervals = std::move(lexicalScan.commentIntervals);
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

/// One producer conditional directive, as the lexical census must find it.
struct ProducerConditionalControl {
  uint64_t end = 0;
  PreprocessingStructureKind kind = PreprocessingStructureKind::OtherDirective;
  uint64_t groupId = 0;
  std::optional<uint64_t> armId;
  bool bound = false;
};

/// Body of one producer conditional arm: from the end of its directive to the
/// `#` of the next directive of its group.
struct ProducerConditionalArmBody {
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t armId = 0;
};

/// Bind lexical conditional controls to the producer's conditional records, and
/// give every interval its enclosing producer arm.
///
/// The producer records every conditional directive the preprocessor read,
/// including those it only scanned inside an excluded block, with the extent
/// Clang's lexer measured.  A lexical control therefore binds to the record
/// whose extent it shares: the same `#` and the same end.  Every lexical
/// control must bind and every producer record must be found, or the index is
/// incomplete.
///
/// The enclosing arm of an interval is the innermost producer arm whose body
/// holds it whole.  Arm bodies nest, and a group's own `#elif`/`#else`/`#endif`
/// lines lie between its arm bodies, so they, like its opening line, belong to
/// the enclosing group's arm.  That is the lexical ownership a nesting stack
/// over the same controls computes.
static void bindConditionalDirectives(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
    std::vector<ScannedDirective> &directives,
    std::vector<std::string> &diagnostics) {
  // Keyed by introducer offset; ordered so unbound records are reported in
  // source order.
  std::map<uint64_t, ProducerConditionalControl> controls;
  std::vector<ProducerConditionalArmBody> bodies;
  auto addControl = [&](uint64_t begin, ProducerConditionalControl control) {
    if (!controls.try_emplace(begin, control).second)
      diagnostics.push_back(
          llvm::formatv("producer conditional directives in group id={0} and "
                        "id={1} share the introducer at byte {2}",
                        controls[begin].groupId, control.groupId, begin)
              .str());
  };
  for (const RefoldModel::CondGroup &group : model.GetConds()) {
    if (!paths.PathsEqual(group.file, sourcePath) ||
        !ownerMatches(group.parentIncludeId, ownerIncludeId))
      continue;
    const bool measured =
        group.endif && llvm::all_of(group.arms, [](const auto &arm) {
          return arm.directive.has_value();
        });
    if (!measured) {
      diagnostics.push_back(
          llvm::formatv("producer conditional group id={0} has no recorded "
                        "directive extents",
                        group.id)
              .str());
      continue;
    }
    for (size_t armIndex = 0; armIndex < group.arms.size(); ++armIndex) {
      const RefoldModel::CondArm &arm = group.arms[armIndex];
      // Arm kinds are spelled as directive keywords.
      const PreprocessingStructureKind kind =
          classifyDirectiveKeyword(arm.kind, /*numericHead=*/false);
      if (!isConditionalControl(kind) ||
          kind == PreprocessingStructureKind::ConditionalEndif) {
        diagnostics.push_back(
            llvm::formatv("producer conditional arm id={0} has unknown kind "
                          "'{1}'",
                          arm.id, arm.kind)
                .str());
        continue;
      }
      addControl(arm.directive->begin,
                 {arm.directive->end, kind, group.id, arm.id});
      const uint64_t bodyEnd = armIndex + 1 < group.arms.size()
                                   ? group.arms[armIndex + 1].directive->begin
                                   : group.endif->begin;
      bodies.push_back({arm.directive->end, bodyEnd, arm.id});
    }
    addControl(group.endif->begin,
               {group.endif->end, PreprocessingStructureKind::ConditionalEndif,
                group.id, std::nullopt});
  }

  for (ScannedDirective &directive : directives) {
    if (!isConditionalControl(directive.interval.kind))
      continue;
    auto found = controls.find(directive.introducerBegin);
    if (found == controls.end() ||
        found->second.end != directive.interval.end ||
        found->second.kind != directive.interval.kind || found->second.bound) {
      diagnostics.push_back(
          llvm::formatv("conditional control {0} range=[{1},{2}) has no unique "
                        "exact producer group/arm binding",
                        toString(directive.interval.kind),
                        directive.interval.begin, directive.interval.end)
              .str());
      continue;
    }
    found->second.bound = true;
    directive.interval.modelKind =
        PreprocessingStructureModelKind::ConditionalDirective;
    directive.interval.conditionalGroupId = found->second.groupId;
    directive.interval.conditionalArmId = found->second.armId;
  }
  for (const auto &entry : controls) {
    if (entry.second.bound)
      continue;
    diagnostics.push_back(
        llvm::formatv("producer conditional directive of group id={0} at byte "
                      "{1} has no exact lexical binding",
                      entry.second.groupId, entry.first)
            .str());
  }

  // Sweep the source-ordered intervals against the nested arm bodies.  `open`
  // holds the bodies that began at or before the current interval and have
  // not yet ended, innermost last.
  llvm::sort(bodies, [](const ProducerConditionalArmBody &lhs,
                        const ProducerConditionalArmBody &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    return lhs.end > rhs.end;
  });
  std::vector<const ProducerConditionalArmBody *> open;
  size_t nextBody = 0;
  for (ScannedDirective &directive : directives) {
    const uint64_t begin = directive.interval.begin;
    const uint64_t end = directive.interval.end;
    for (; nextBody < bodies.size() && bodies[nextBody].begin <= begin;
         ++nextBody) {
      while (!open.empty() && open.back()->end <= bodies[nextBody].begin)
        open.pop_back();
      open.push_back(&bodies[nextBody]);
    }
    while (!open.empty() && open.back()->end <= begin)
      open.pop_back();
    for (auto body = open.rbegin(); body != open.rend(); ++body) {
      if (end <= (*body)->end) {
        directive.interval.ownerConditionalArmId = (*body)->armId;
        break;
      }
    }
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
static void bindMacroDirectives(const RefoldModel &model,
                                const RefoldPathIdentity &paths,
                                StringRef sourcePath, StringRef sourceBytes,
                                std::optional<uint64_t> ownerIncludeId,
                                std::vector<ScannedDirective> &directives,
                                std::vector<std::string> &diagnostics) {
  for (const RefoldModel::MacroDirective &modelDirective :
       model.GetMacroDirectives()) {
    if (!paths.PathsEqual(modelDirective.sitePath, sourcePath) ||
        !ownerMatches(modelDirective.ownerIncludeId, ownerIncludeId))
      continue;

    std::optional<MacroStateDirectiveLineInterval> recovered =
        recoverMacroStateDirectiveLineInterval(
            paths, modelDirective, sourcePath, sourceBytes, ownerIncludeId);
    if (!recovered) {
      diagnostics.push_back(
          llvm::formatv("macro directive id={0} has no exact full-line source "
                        "interval",
                        modelDirective.id)
              .str());
      continue;
    }

    const PreprocessingStructureKind expectedKind =
        modelDirective.IsDefine() ? PreprocessingStructureKind::MacroDefine
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

    // A producer-recorded physical extent and this scanner measure the same
    // thing by two independent routes: Clang's own directive introducer token
    // plus the lexer position past the directive, against a language-mode-aware
    // lexical scan of the file bytes.  Requiring exact agreement keeps the
    // recorded extent a fact rather than a second opinion, and it is checked
    // here because this is the only place both are in hand.
    //
    // The recorded begin is the `#` itself, so it is compared against
    // introducerBegin rather than against the lexical interval begin, which
    // also covers any leading trivia on the logical-line prefix.
    if (recovered->begin != scanned.introducerBegin ||
        recovered->end != scanned.interval.end) {
      diagnostics.push_back(
          llvm::formatv("macro directive id={0} recorded physical extent "
                        "[{1},{2}) disagrees with the scanned logical line "
                        "introducer={3} end={4}",
                        modelDirective.id, recovered->begin, recovered->end,
                        scanned.introducerBegin, scanned.interval.end)
              .str());
      continue;
    }

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
///
/// The producer's `directiveLine` and this scanner measure the same directive
/// by two independent routes: Clang's lexer position after the end-of-directive
/// token, against a language-mode-aware lexical scan of the file bytes.  As for
/// macro-state directives, only exact agreement binds the record.  The
/// directive's `text` plays no part: the producer synthesizes it, so it matches
/// the source only for a canonically spelled directive.
static void bindIncludeDirectives(
    const RefoldModel &model, const RefoldPathIdentity &paths,
    StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
    std::vector<ScannedDirective> &directives,
    std::vector<std::string> &diagnostics) {
  for (const RefoldModel::IncludeItem &include : model.GetIncludes()) {
    if (!paths.PathsEqual(include.sitePath, sourcePath) ||
        !ownerMatches(include.parent, ownerIncludeId))
      continue;

    if (!include.directiveLine) {
      diagnostics.push_back(
          llvm::formatv("include directive id={0} has no recorded directive "
                        "extent",
                        include.id)
              .str());
      continue;
    }
    const RefoldModel::ByteRange &line = *include.directiveLine;

    const PreprocessingStructureKind expectedKind =
        includeStructureKind(include);
    std::optional<size_t> scannedIndex = findUniqueDirectiveForProducerRange(
        directives, line.begin, line.end, expectedKind);
    if (!scannedIndex) {
      diagnostics.push_back(
          llvm::formatv("include directive id={0} recorded range=[{1},{2}) "
                        "does not select one lexical {3} directive",
                        include.id, line.begin, line.end,
                        toString(expectedKind))
              .str());
      continue;
    }

    ScannedDirective &scanned = directives[*scannedIndex];
    if (line.begin != scanned.introducerBegin ||
        line.end != scanned.interval.end) {
      diagnostics.push_back(
          llvm::formatv("include directive id={0} recorded extent [{1},{2}) "
                        "disagrees with the scanned logical line "
                        "introducer={3} end={4}",
                        include.id, line.begin, line.end,
                        scanned.introducerBegin, scanned.interval.end)
              .str());
      continue;
    }
    attachModelBinding(scanned,
                       PreprocessingStructureModelKind::IncludeDirective,
                       include.id, line.begin, line.end, diagnostics);
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

/// Return the complete physical source interval of a recorded macro-state
/// directive line.
///
/// The interval is the producer's recorded physical extent: Clang's directive
/// introducer through where its lexer stood after the end-of-directive token.
/// MacroDirective::text cannot stand in for it, because it is rendered from
/// parsed macro tokens with canonical spacing and no transform recovers the
/// source bytes from a pretty-printer's output.
std::optional<MacroStateDirectiveLineInterval>
recoverMacroStateDirectiveLineInterval(
    const RefoldPathIdentity &paths,
    const RefoldModel::MacroDirective &directive, StringRef expectedPath,
    StringRef fileBytes, std::optional<uint64_t> requiredOwnerIncludeId) {
  if (!directive.IsMacroStateDirective())
    return std::nullopt;
  if (directive.name.empty())
    return std::nullopt;
  if (!paths.PathsEqual(directive.sitePath, expectedPath))
    return std::nullopt;

  if (requiredOwnerIncludeId) {
    if (!directive.ownerIncludeId ||
        *directive.ownerIncludeId != *requiredOwnerIncludeId)
      return std::nullopt;
  } else if (directive.ownerIncludeId) {
    return std::nullopt;
  }

  // The model parser already proved the pair is ordered and contains the
  // name-anchored site range; only its fit to these particular file bytes
  // remains to be checked here, because the caller supplies the buffer.
  if (directive.directiveLineE > fileBytes.size())
    return std::nullopt;

  MacroStateDirectiveLineInterval result;
  result.directive = &directive;
  result.begin = directive.directiveLineB;
  result.end = directive.directiveLineE;
  result.name = directive.name;
  return result;
}

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
      index.indivisibleTriviaIntervals_, index.commentIntervals_,
      protectionDiagnostics);

  // Supplement directly scanned `_Pragma` expressions with producer-proven
  // expansion-derived occurrences before source ordering and conditional
  // topology.  Both forms then receive the same exact enclosing-arm ownership
  // as ordinary directive lines.
  appendPragmaOperatorIntervals(deps.model, deps.paths, sourcePath, sourceBytes,
                                ownerIncludeId, directives,
                                protectionDiagnostics);
  llvm::sort(directives, scannedDirectiveLess);

  bindConditionalDirectives(deps.model, deps.paths, sourcePath, ownerIncludeId,
                            directives, protectionDiagnostics);
  bindMacroDirectives(deps.model, deps.paths, sourcePath, sourceBytes,
                      ownerIncludeId, directives, index.diagnostics_);
  bindIncludeDirectives(deps.model, deps.paths, sourcePath, ownerIncludeId,
                        directives, index.diagnostics_);
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

size_t RefoldPreprocessingStructureIndex::FirstPossibleOverlappingIndex(
    uint64_t begin) const {
  auto firstPossible = std::upper_bound(prefixMaximumIntervalEnds_.begin(),
                                        prefixMaximumIntervalEnds_.end(),
                                        begin);
  return static_cast<size_t>(
      std::distance(prefixMaximumIntervalEnds_.begin(), firstPossible));
}

std::vector<const PreprocessingStructureInterval *>
RefoldPreprocessingStructureIndex::FindOverlapping(uint64_t begin,
                                                   uint64_t end) const {
  std::vector<const PreprocessingStructureInterval *> result;
  if (end <= begin)
    return result;

  for (size_t intervalIndex = FirstPossibleOverlappingIndex(begin);
       intervalIndex < intervals_.size(); ++intervalIndex) {
    const PreprocessingStructureInterval &interval =
        intervals_[intervalIndex];
    if (interval.begin >= end)
      break;
    if (interval.Overlaps(begin, end))
      result.push_back(&interval);
  }
  return result;
}

bool RefoldPreprocessingStructureIndex::HasOverlapping(uint64_t begin,
                                                       uint64_t end) const {
  if (end <= begin)
    return false;

  for (size_t intervalIndex = FirstPossibleOverlappingIndex(begin);
       intervalIndex < intervals_.size(); ++intervalIndex) {
    const PreprocessingStructureInterval &interval =
        intervals_[intervalIndex];
    if (interval.begin >= end)
      break;
    if (interval.Overlaps(begin, end))
      return true;
  }
  return false;
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

bool RefoldPreprocessingStructureIndex::RangeContainsComment(
    uint64_t begin, uint64_t end) const {
  return !CommentsWithin(begin, end).empty();
}

ArrayRef<PreprocessingTriviaInterval>
RefoldPreprocessingStructureIndex::CommentsWithin(uint64_t begin,
                                                  uint64_t end) const {
  // Comments never overlap, so the ones starting at or after `begin` are
  // sorted by `end` as well and those ending by `end` form a prefix.
  auto first = std::lower_bound(
      commentIntervals_.begin(), commentIntervals_.end(), begin,
      [](const PreprocessingTriviaInterval &interval, uint64_t boundary) {
        return interval.begin < boundary;
      });
  auto last =
      std::partition_point(first, commentIntervals_.end(),
                           [&](const PreprocessingTriviaInterval &interval) {
                             return interval.end <= end;
                           });
  return ArrayRef<PreprocessingTriviaInterval>(commentIntervals_)
      .slice(first - commentIntervals_.begin(), last - first);
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
  if (HasOverlapping(begin, end))
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

std::optional<StringRef>
RefoldPreprocessingStructureIndex::BoundMacroStateDirectiveSourceText(
    const RefoldModel::MacroDirective &directive, StringRef sourceBytes) const {
  const uint64_t begin = directive.directiveLineB;
  const uint64_t end = directive.directiveLineE;
  if (sourceBytes.size() != sourceSize_ || end <= begin || end > sourceSize_)
    return std::nullopt;

  // A bound interval contains its producer range, which is the recorded
  // extent, so it overlaps that extent.  An unbound one carries no model
  // record and is skipped.
  const PreprocessingStructureInterval *bound = nullptr;
  for (const PreprocessingStructureInterval *interval :
       FindOverlapping(begin, end)) {
    if (!isExactProducerBoundMacroStateInterval(*interval) ||
        interval->modelItemId != directive.id)
      continue;
    if (bound)
      return std::nullopt;
    bound = interval;
  }
  if (!bound || *bound->producerTextBegin != begin ||
      *bound->producerTextEnd != end)
    return std::nullopt;
  return sourceBytes.slice(begin, end);
}

} // namespace refold
} // namespace clang

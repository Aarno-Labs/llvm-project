//===--- RefoldMacroStateProof.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Macro-state proof service implementation.
//
// This file implements #define/#undef observation predicates, directive-line
// interval recovery, source-neutral macro-state gap checks, and materialized-
// header macro-state stabilization helpers.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroStateProof.h"

#include "core/RefoldLog.h"
#include "proof/RefoldOwnerStateProof.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

/// Classify the source observation needed to make `directive` visible.
///
/// Object-like definitions and #undef transitions are conservative identifier
/// observations.  Function-like #defines use the narrower NAME(`...`)
/// observation class, because a bare identifier spelling does not invoke them.
MacroStateObservationKind
RefoldMacroStateProof::MacroStateObservationKindForDirective(
    const RefoldModel::MacroDirective &directive, StringRef macroName) const {
  if (directive.subkind == "#define" && directive.name == macroName &&
      directive.functionLike) {
    return MacroStateObservationKind::FunctionLikeInvocation;
  }
  return MacroStateObservationKind::IdentifierToken;
}

/// Return the first byte offset in `text` that would observe the macro-state
/// transition represented by `directive`.
///
/// This centralizes the object-like/function-like distinction so TU repair,
/// include materialization, and fallback proofs apply the same observation
/// invariant.
std::optional<size_t>
RefoldMacroStateProof::FirstMacroStateObservationOffsetInText(
    const RefoldModel::MacroDirective &directive, StringRef macroName,
    StringRef text, StringRef suffix) const {
  switch (MacroStateObservationKindForDirective(directive, macroName)) {
  case MacroStateObservationKind::IdentifierToken:
    return tokenText_.FirstRawIdentifierObservationOffsetInText(macroName,
                                                                text);
  case MacroStateObservationKind::FunctionLikeInvocation:
    return tokenText_.FirstFunctionLikeInvocationOffsetInText(macroName, text,
                                                              suffix);
  }
  return std::nullopt;
}

/// True when a replacement payload would be preprocessed differently if
/// `directive` were active before that payload.
bool RefoldMacroStateProof::ReplacementObservesMacroStateDirective(
    const RefoldModel::MacroDirective &directive, StringRef replacement,
    bool unprovenObserves) const {
  if (directive.subkind != "#define" && directive.subkind != "#undef")
    return false;
  if (directive.name.empty())
    return unprovenObserves;
  return FirstMacroStateObservationOffsetInText(directive, directive.name,
                                                replacement)
      .has_value();
}

bool RefoldMacroStateProof::MacroDefinitionIsSelfReferentialIdentity(
    const RefoldModel::MacroDirective &directive) const {
  if (directive.subkind != "#define" || directive.name.empty())
    return false;
  // A function-like macro only expands before `(`, and its replacement list is
  // reached through argument substitution.  The identity argument below is
  // about a bare name expanding to itself, so it does not cover that shape.
  if (directive.functionLike || !directive.defParams.empty())
    return false;
  if (directive.replacementTokens.size() != 1)
    return false;
  const RefoldModel::MacroReplacementToken &token =
      directive.replacementTokens.front();
  return token.kind == RefoldModel::MacroReplacementTokenKind::Literal &&
         !token.paramIndex && token.spelling == directive.name;
}

bool RefoldMacroStateProof::SelfReferentialDefinitionIsUnobservableInText(
    const RefoldModel::MacroDirective &directive, StringRef macroName,
    StringRef text) const {
  if (!MacroDefinitionIsSelfReferentialIdentity(directive))
    return false;
  if (macroName.empty() || macroName != directive.name)
    return false;
  if (text.empty())
    return true;

  // Only a directive line can spell a conditional test, so the payload's
  // directive inventory is the complete set of places where activating the
  // definition could be observed as something other than the identity.  An
  // incomplete scan reports named, which keeps this fail-closed.
  if (tokenText_.DirectiveLineInTextCouldObserveDefinedness(macroName, text)) {
    REFOLD_LOG_TRACE("macro/state",
                     "self-referential '{0}' is named by a payload directive "
                     "line that could observe its definedness",
                     macroName);
    return false;
  }

  REFOLD_LOG_TRACE("macro/state",
                   "self-referential '{0}' expands to itself and is named by no "
                   "directive line in the payload, so activating it before the "
                   "payload cannot change how it preprocesses",
                   macroName);
  return true;
}

/// True when moving `directive` across `chunk` could change how that chunk
/// preprocesses.
///
/// `following` is part of the proof for function-like definitions: NAME at the
/// end of `chunk` followed by `(` in the following source still observes the
/// definition across the movement boundary.
bool RefoldMacroStateProof::SourceChunkObservesMacroStateDirectiveWhenCrossed(
    const RefoldModel::MacroDirective &directive, StringRef macroName,
    StringRef chunk, StringRef following) const {
  if (chunk.empty())
    return false;
  if (tokenText_.TextContainsDirectiveLine(chunk))
    return true;
  return FirstMacroStateObservationOffsetInText(directive, macroName, chunk,
                                                following)
      .has_value();
}

/// Recover the complete physical source interval for a recorded macro-state
/// directive line.
///
/// When the producer recorded the directive's physical extent, that pair *is*
/// the interval and is returned directly.  Otherwise the interval is recovered
/// from the recorded spelling: MacroDirective::siteB is anchored at the macro
/// name rather than at the `#`, so this reparses MacroDirective::text to find
/// the name offset, translates that anchor back to the line start, and accepts
/// the result only if the file bytes there equal the recorded text exactly.
///
/// The two paths are not interchangeable in strength.  MacroDirective::text is
/// rendered from parsed macro tokens with canonical spacing, so the text
/// comparison is a proof only for a directive whose source spelling already
/// matches that rendering; it necessarily fails for tabs, runs of spaces, and
/// backslash continuations, and no transform recovers the source bytes from a
/// pretty-printer's output.  The recorded extent replaces that inference with a
/// fact and therefore does not need the comparison.
std::optional<MacroStateDirectiveLineInterval>
RefoldMacroStateProof::RecoverMacroStateDirectiveLineInterval(
    const RefoldModel::MacroDirective &directive, StringRef expectedPath,
    StringRef fileBytes, std::optional<uint64_t> requiredOwnerIncludeId) const {
  if (directive.subkind != "#define" && directive.subkind != "#undef")
    return std::nullopt;
  if (directive.name.empty() || directive.text.empty())
    return std::nullopt;
  if (!paths_.PathsEqual(directive.sitePath, expectedPath))
    return std::nullopt;

  if (requiredOwnerIncludeId) {
    if (!directive.ownerIncludeId ||
        *directive.ownerIncludeId != *requiredOwnerIncludeId)
      return std::nullopt;
  } else if (directive.ownerIncludeId) {
    return std::nullopt;
  }

  // Producer-recorded physical extent.  The model parser already proved the
  // pair is ordered and contains the name-anchored site range; only its fit to
  // these particular file bytes remains to be checked here, because the caller
  // supplies the buffer.
  if (directive.directiveLineB && directive.directiveLineE) {
    if (*directive.directiveLineE > fileBytes.size())
      return std::nullopt;

    MacroStateDirectiveLineInterval recorded;
    recorded.directive = &directive;
    recorded.begin = *directive.directiveLineB;
    recorded.end = *directive.directiveLineE;
    recorded.name = directive.name;
    return recorded;
  }

  StringRef text = directive.text;
  size_t pos = 0;
  stringutils::skipNonNewlineWs(text, pos);
  if (pos >= text.size() || text[pos] != '#')
    return std::nullopt;
  ++pos;
  stringutils::skipNonNewlineWs(text, pos);

  StringRef keyword = directive.subkind.drop_front();
  if (!text.substr(pos).starts_with(keyword))
    return std::nullopt;
  pos += keyword.size();
  if (pos < text.size() && stringutils::isIdentPart(text[pos]))
    return std::nullopt;
  stringutils::skipNonNewlineWs(text, pos);

  const size_t nameTextBegin = pos;
  if (pos >= text.size() || !stringutils::isIdentStart(text[pos]))
    return std::nullopt;
  ++pos;
  while (pos < text.size() && stringutils::isIdentPart(text[pos]))
    ++pos;
  if (text.slice(nameTextBegin, pos) != directive.name)
    return std::nullopt;

  if (directive.siteB < nameTextBegin)
    return std::nullopt;
  const uint64_t fileBegin = directive.siteB - nameTextBegin;
  const uint64_t fileEnd = fileBegin + text.size();
  if (fileBegin >= fileEnd || fileEnd > fileBytes.size())
    return std::nullopt;
  if (fileBytes.slice(fileBegin, fileEnd) != text)
    return std::nullopt;

  MacroStateDirectiveLineInterval result;
  result.directive = &directive;
  result.begin = fileBegin;
  result.end = fileEnd;
  result.name = directive.name;
  return result;
}

/// Recover the source interval occupied by the replacement list of the macro
/// definition used by `invocation`.
///
/// The interval is used by directive-repair logic that needs to reason about
/// preserving or replaying only the definition body, not the `#define
/// NAME(...)` prefix.  The parse is intentionally shallow and fail-closed: it
/// recognizes the directive prefix and balanced function-like parameter list,
/// then returns the remaining replacement-list bytes.
std::optional<MacroDefinitionReplacementListInterval>
RefoldMacroStateProof::RecoverMacroDefinitionReplacementListInterval(
    const RefoldModel::MacroInvocation &invocation) const {
  if (!invocation.definitionDirectiveId)
    return std::nullopt;

  const RefoldModel::MacroDirective *definition =
      model_.GetMacroDirectiveById(*invocation.definitionDirectiveId);

  if (!definition || definition->subkind != "#define")
    return std::nullopt;
  if (definition->name != invocation.name)
    return std::nullopt;

  StringRef text = definition->text;
  size_t pos = 0;
  stringutils::skipNonNewlineWs(text, pos);
  if (pos >= text.size() || text[pos] != '#')
    return std::nullopt;
  ++pos;
  stringutils::skipNonNewlineWs(text, pos);

  if (!text.substr(pos).starts_with("define"))
    return std::nullopt;
  pos += StringRef("define").size();
  if (pos < text.size() && stringutils::isIdentPart(text[pos]))
    return std::nullopt;
  stringutils::skipNonNewlineWs(text, pos);

  const size_t nameTextBegin = pos;
  if (!text.substr(pos).starts_with(invocation.name))
    return std::nullopt;
  pos += invocation.name.size();
  if (pos < text.size() && stringutils::isIdentPart(text[pos]))
    return std::nullopt;

  if (invocation.subkind == "func") {
    if (pos >= text.size() || text[pos] != '(')
      return std::nullopt;

    // Skip the function-like parameter list using only parenthesis balance.
    // The macro replacement list begins at the first byte after the matching
    // `)`, including any horizontal whitespace that the original source kept.
    unsigned depth = 0;
    while (pos < text.size()) {
      char ch = text[pos++];
      if (ch == '(') {
        ++depth;
        continue;
      }
      if (ch == ')') {
        if (depth == 0)
          return std::nullopt;
        --depth;
        if (depth == 0)
          break;
      }
    }
    if (depth != 0)
      return std::nullopt;
  }

  if (definition->siteB < nameTextBegin)
    return std::nullopt;

  MacroDefinitionReplacementListInterval result;
  result.directive = definition;
  result.nameTextBegin = nameTextBegin;
  result.replacementTextBegin = pos;
  // Text offsets are converted to source offsets through `fileBase`, which is
  // only meaningful if the recorded directive prefix is the source prefix
  // byte-for-byte.  The producer's physical extent is what can settle that: it
  // names the real `#`, so agreement proves the conversion and disagreement
  // proves the recorded spelling is a re-rendering this helper cannot invert.
  result.fileBase = definition->siteB - nameTextBegin;
  result.fileBegin = result.fileBase + pos;

  if (definition->directiveLineB && definition->directiveLineE) {
    if (*definition->directiveLineB != result.fileBase)
      return std::nullopt;

    // The replacement list runs to the end of the directive's logical line.
    // Bounding it by the recorded text length instead would reject every
    // definition whose source line carries bytes the rendering drops -- a
    // trailing comment, or a backslash continuation -- because those make the
    // source strictly longer than its canonical spelling.
    result.fileEnd = *definition->directiveLineE;
    if (result.fileBegin > result.fileEnd)
      return std::nullopt;
    return result;
  }

  // Without a recorded extent there is no way to tell a faithful rendering from
  // a re-rendering, so keep the historical bound: the source span must fit
  // inside the recorded spelling.  That admits only canonically spelled
  // definitions, which is the most this evidence supports.
  result.fileEnd = definition->siteE;
  if (result.fileBegin > result.fileEnd ||
      result.fileEnd > result.fileBase + text.size())
    return std::nullopt;
  return result;
}

std::optional<StabilizedMaterializedHeaderMacroPatch>
RefoldMacroStateProof::StabilizeMaterializedHeaderMacroPatchReplay(
    MacroStatePatchReplayInput mp, uint64_t mpEnd, StringRef headerPath,
    uint64_t includeId, StringRef bytes,
    ArrayRef<MacroStateStagedEditInterval> stagedEdits) const {
  using MaterializedHeaderMacroStateDirectivePiece =
      MacroStateDirectiveLineInterval;

  auto materializedHeaderMacroStateBoundary = [&](uint64_t begin,
                                                  uint64_t end) {
    return OwnerStateBoundary::FromSource(OwnerSourceRange::From(
        headerPath, begin, end, std::optional<uint64_t>(includeId)));
  };

  auto checkMaterializedHeaderMacroState =
      [&](const OwnerStateBoundary &boundary, StateMutationKind mutation,
          SuffixStabilityWitness witness, StringRef stage, StringRef detail,
          bool requireKnownObserver) {
        return ownerStateProof_.CheckStateTransitionAcrossEditBoundary(
            boundary, OwnerStateComponent::MacroState, mutation,
            std::move(witness), stage, detail, requireKnownObserver);
      };

  auto failMaterializedHeaderMacroState =
      [&](const OwnerStateBoundary &boundary, StateMutationKind mutation,
          StringRef detail) {
        return checkMaterializedHeaderMacroState(
            boundary, mutation,
            ownerStateProof_.BuildStateTransitionWitness(
                SuffixStabilityWitnessKind::TerminalStateFailure,
                OwnerStateComponent::MacroState, boundary, detail),
            "include/materialized-macro-state", detail,
            /*requireKnownObserver=*/true);
      };

  auto widenMaterializedHeaderMacroState =
      [&](const OwnerStateBoundary &boundary, StateMutationKind mutation,
          StringRef detail) {
        return checkMaterializedHeaderMacroState(
            boundary, mutation,
            ownerStateProof_.BuildStateTransitionWitness(
                SuffixStabilityWitnessKind::ClosureWidening,
                OwnerStateComponent::MacroState, boundary, detail),
            "include/materialized-macro-state", detail,
            /*requireKnownObserver=*/false);
      };

  auto materializedHeaderMacroStateDirectiveInterval =
      [&](const RefoldModel::MacroDirective &directive) {
        return RecoverMacroStateDirectiveLineInterval(
            directive, headerPath, bytes, std::optional<uint64_t>(includeId));
      };

  // Reconstruct the active definition for `macroName` immediately before a
  // header byte offset using only directives owned by this materialized
  // include. The last matching #define/#undef before the offset wins, with
  // directive id as a deterministic tie-breaker for equal byte endpoints.
  auto activeMaterializedHeaderDefinitionAtByte =
      [&](const RefoldModel::MacroDirective &definition, StringRef macroName,
          uint64_t offset) {
        const RefoldModel::MacroDirective *active = nullptr;
        uint64_t activeEnd = 0;
        for (const auto &candidate : model_.GetMacroDirectives()) {
          std::optional<MaterializedHeaderMacroStateDirectivePiece> piece =
              materializedHeaderMacroStateDirectiveInterval(candidate);
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

  // A macro-state repair interval must not overlap an edit already staged for
  // this materialized header.  Overlap would require composing two proof
  // artifacts, which belongs to the global edit-composition layer rather than
  // this local replay-stability repair.
  auto materializedHeaderRangeOverlapsStagedEdit = [&](uint64_t begin,
                                                       uint64_t end) {
    for (const MacroStateStagedEditInterval &edit : stagedEdits)
      if (begin < edit.end && end > edit.start)
        return true;
    return false;
  };

  // Extend a macro patch to the end of its physical line before appending
  // carried directive lines.  This guarantees that moved #define lines remain
  // real preprocessing directives, not tokens pasted onto the invocation line.
  auto materializedHeaderLineEndAfter = [&](uint64_t offset) {
    uint64_t p = std::min<uint64_t>(offset, bytes.size());
    while (p < bytes.size() && bytes[static_cast<size_t>(p)] != '\n')
      ++p;
    if (p < bytes.size())
      ++p;
    return p;
  };

  if (mp.invStart > mp.invEnd || mp.invEnd > bytes.size() ||
      mpEnd < mp.invEnd || mpEnd > bytes.size())
    return std::nullopt;

  // Collect active #define lines that the B replacement would observe if
  // emitted at the original invocation site.  Only movable whole directives
  // become candidates; if an observed definition exists but cannot be moved,
  // the proof fails closed below.
  SmallVector<MaterializedHeaderMacroStateDirectivePiece, 4> candidates;
  bool observedUncarriedDefinition = false;
  for (const auto &directive : model_.GetMacroDirectives()) {
    if (directive.subkind != "#define")
      continue;
    std::optional<MaterializedHeaderMacroStateDirectivePiece> piece =
        materializedHeaderMacroStateDirectiveInterval(directive);
    if (!piece || piece->end > mp.invStart)
      continue;
    if (!activeMaterializedHeaderDefinitionAtByte(directive, piece->name,
                                                  mp.invStart))
      continue;
    if (!ReplacementObservesMacroStateDirective(directive,
                                                StringRef(mp.replacement),
                                                /*unprovenObserves=*/false))
      continue;

    observedUncarriedDefinition = true;
    if (materializedHeaderRangeOverlapsStagedEdit(piece->begin, piece->end))
      continue;
    candidates.push_back(std::move(*piece));
  }

  if (candidates.empty()) {
    if (!observedUncarriedDefinition)
      return std::nullopt;
    (void)failMaterializedHeaderMacroState(
        materializedHeaderMacroStateBoundary(mp.invStart, mpEnd),
        StateMutationKind::Materialized,
        llvm::formatv("include-owned macro patch in {0} observes active "
                      "header macro state before [{1},{2}) but no movable "
                      "definition line was proven",
                      headerPath, mp.invStart, mpEnd)
            .str());
    return StabilizedMaterializedHeaderMacroPatch{};
  }

  // Emit carried directives in source order so the relative macro-state
  // transition order among multiple carried definitions is preserved.
  llvm::sort(candidates,
             [](const MaterializedHeaderMacroStateDirectivePiece &lhs,
                const MaterializedHeaderMacroStateDirectivePiece &rhs) {
               if (lhs.begin != rhs.begin)
                 return lhs.begin < rhs.begin;
               return lhs.directive->id < rhs.directive->id;
             });

  // Widen from the first carried directive through the completed invocation
  // line.  The suffix after the invocation is consumed and replayed before the
  // carried directives, which keeps the directives line-started after repair.
  const uint64_t newStart = candidates.front().begin;
  const uint64_t newEnd = materializedHeaderLineEndAfter(mpEnd);
  if (newStart > mp.invStart || newEnd < mpEnd || newEnd > bytes.size() ||
      materializedHeaderRangeOverlapsStagedEdit(newStart, newEnd)) {
    (void)failMaterializedHeaderMacroState(
        materializedHeaderMacroStateBoundary(newStart, newEnd),
        StateMutationKind::WidenedIntoClosure,
        llvm::formatv("include-owned macro patch in {0} cannot widen "
                      "macro-state repair interval [{1},{2})",
                      headerPath, newStart, newEnd)
            .str());
    return StabilizedMaterializedHeaderMacroPatch{};
  }

  // Helper used while rebuilding the replacement to distinguish directive
  // bytes intentionally moved to the end from ordinary source bytes that must
  // remain in their original relative order.
  auto isCarriedDirectiveInterval = [&](uint64_t begin, uint64_t end) {
    for (const auto &candidate : candidates)
      if (candidate.begin == begin && candidate.end == end)
        return true;
    return false;
  };

  // Build the source text that a carried directive crosses, excluding the
  // carried directive lines themselves.  That text is the proof obligation for
  // non-observation: if it observes the definition being delayed, movement is
  // not semantics-preserving.
  auto appendCrossedSourceExcludingCarried = [&](std::string &out,
                                                 uint64_t begin, uint64_t end) {
    uint64_t cursor = begin;
    for (const auto &candidate : candidates) {
      if (candidate.end <= cursor || candidate.begin >= end)
        continue;
      if (cursor < candidate.begin)
        out.append(bytes.begin() + cursor, bytes.begin() + candidate.begin);
      cursor = std::max(cursor, candidate.end);
    }
    if (cursor < end)
      out.append(bytes.begin() + cursor, bytes.begin() + end);
  };

  // The replacement payload will be emitted before each carried definition,
  // and the original line suffix after the macro invocation must also remain
  // before those definitions so the directives still start on real directive
  // lines.  Therefore every byte crossed by each carried definition must be
  // proven non-observing for that definition.  Otherwise the macro-state move
  // would change how preserved header source preprocesses.
  // Alternative repair, attempted only when a definition cannot be carried:
  // leave every definition exactly where it is, undefine the observed ones
  // immediately before the replacement's own line, and restore them immediately
  // after it from the producer's directive text.
  //
  // The restore is what makes this self-contained.  Macro state after the
  // repaired interval is identical to the state before it, so no obligation
  // falls on the header suffix or on translation-unit source after the include
  // -- neither of which this proof can see.  Only the repaired line changes
  // what it observes, and the three chunks that line is made of are each
  // proven indifferent to the name below.
  auto buildUndefRestoreMaterializedHeaderPatch =
      [&]() -> std::optional<StabilizedMaterializedHeaderMacroPatch> {
    const uint64_t lineStart = static_cast<uint64_t>(
        stringutils::lineStartOffset(bytes, static_cast<size_t>(mp.invStart)));
    if (lineStart > mp.invStart)
      return std::nullopt;

    // A spliced physical line continues a logical line that began earlier, so a
    // `#undef` inserted at this offset would not start a directive line.
    if (lineStart > 0 &&
        stringutils::isLineSplice(bytes, static_cast<size_t>(lineStart) - 1))
      return std::nullopt;

    const uint64_t restoreEnd = materializedHeaderLineEndAfter(mpEnd);
    if (restoreEnd < mpEnd || restoreEnd > bytes.size())
      return std::nullopt;
    if (materializedHeaderRangeOverlapsStagedEdit(lineStart, restoreEnd))
      return std::nullopt;

    // A directive inside the replacement may select an arm whose conditional
    // state is itself part of the owner proof, which is no longer a plain
    // token repair.  A B-derived payload carries no directive, so this excludes
    // only cases this repair was never meant to cover.
    if (tokenText_.TextContainsDirectiveLine(StringRef(mp.replacement)))
      return std::nullopt;

    const StringRef sameLinePrefix = bytes.slice(lineStart, mp.invStart);
    const StringRef sameLineSuffix = bytes.slice(mpEnd, restoreEnd);

    for (const auto &candidate : candidates) {
      // The definition must be complete before the repaired line, or the
      // synthesized `#undef` would precede the `#define` it undoes.
      if (candidate.end > lineStart)
        return std::nullopt;

      // The prefix is preprocessed after the `#undef` and the suffix before the
      // restore, so both see the name undefined where they originally saw it
      // defined.  Neither may observe it.
      if (SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *candidate.directive, candidate.name, sameLinePrefix,
              StringRef(mp.replacement))) {
        return std::nullopt;
      }
      if (SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *candidate.directive, candidate.name, sameLineSuffix,
              StringRef())) {
        return std::nullopt;
      }
    }

    std::string replacement;
    for (const auto &candidate : candidates)
      replacement += (Twine("#undef ") + candidate.name + "\n").str();
    replacement.append(bytes.begin() + lineStart, bytes.begin() + mp.invStart);
    replacement += mp.replacement;
    replacement.append(bytes.begin() + mpEnd, bytes.begin() + restoreEnd);
    if (!replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    // Restore in source order, so several definitions re-establish the same
    // last-one-wins order they had originally.
    for (const auto &candidate : candidates) {
      replacement.append(candidate.directive->text.begin(),
                         candidate.directive->text.end());
      if (replacement.empty() || replacement.back() != '\n')
        replacement.push_back('\n');
    }

    for (const auto &candidate : candidates) {
      (void)widenMaterializedHeaderMacroState(
          materializedHeaderMacroStateBoundary(lineStart, restoreEnd),
          StateMutationKind::WidenedIntoClosure,
          llvm::formatv("include-owned macro patch in {0} undefines '{1}' "
                        "(#{2}) across replay interval [{3},{4}) and restores "
                        "it immediately after",
                        headerPath, candidate.name, candidate.directive->id,
                        lineStart, restoreEnd)
              .str());
    }

    StabilizedMaterializedHeaderMacroPatch stabilized;
    stabilized.start = lineStart;
    stabilized.end = restoreEnd;
    stabilized.replacement = std::move(replacement);
    return stabilized;
  };

  for (const auto &candidate : candidates) {
    std::string crossed;
    appendCrossedSourceExcludingCarried(crossed, candidate.end, mp.invStart);
    appendCrossedSourceExcludingCarried(crossed, mpEnd, newEnd);
    StringRef following = bytes.slice(mp.invStart, mpEnd);
    if (SourceChunkObservesMacroStateDirectiveWhenCrossed(
            *candidate.directive, candidate.name, StringRef(crossed),
            following)) {
      if (std::optional<StabilizedMaterializedHeaderMacroPatch> undefRestore =
              buildUndefRestoreMaterializedHeaderPatch()) {
        return undefRestore;
      }
      (void)failMaterializedHeaderMacroState(
          materializedHeaderMacroStateBoundary(candidate.begin, candidate.end),
          StateMutationKind::MovedLater,
          llvm::formatv("include-owned macro patch in {0} cannot carry "
                        "#{1} for '{2}' across observing header source, and "
                        "cannot undefine and restore it around the replacement",
                        headerPath, candidate.directive->id, candidate.name)
              .str());
      return StabilizedMaterializedHeaderMacroPatch{};
    }
  }

  // Rebuild the widened interval in the only order that preserves B's replay
  // context and the surviving suffix context:
  //
  //   1. original source before each carried definition,
  //   2. original source between the last carried definition and callsite,
  //   3. the B replacement text,
  //   4. the original physical-line suffix after the callsite, and
  //   5. the carried #define lines.
  //
  // Thus the B replacement is evaluated before the carried definitions, while
  // later surviving source sees those definitions restored.
  std::string replacement;
  replacement.reserve((mp.invStart - newStart) + mp.replacement.size() +
                      (newEnd - mpEnd) + candidates.size() * 32);
  uint64_t cursor = newStart;
  for (const auto &candidate : candidates) {
    if (!isCarriedDirectiveInterval(candidate.begin, candidate.end) ||
        cursor > candidate.begin) {
      (void)failMaterializedHeaderMacroState(
          materializedHeaderMacroStateBoundary(newStart, newEnd),
          StateMutationKind::MovedLater,
          llvm::formatv("include-owned macro patch in {0} found invalid "
                        "carried directive ordering",
                        headerPath)
              .str());
      return StabilizedMaterializedHeaderMacroPatch{};
    }
    replacement.append(bytes.begin() + cursor, bytes.begin() + candidate.begin);
    cursor = candidate.end;
  }
  replacement.append(bytes.begin() + cursor, bytes.begin() + mp.invStart);
  replacement += mp.replacement;
  replacement.append(bytes.begin() + mpEnd, bytes.begin() + newEnd);
  if (!replacement.empty() && replacement.back() != '\n')
    replacement.push_back('\n');
  for (const auto &candidate : candidates) {
    replacement.append(candidate.directive->text.begin(),
                       candidate.directive->text.end());
    if (replacement.empty() || replacement.back() != '\n')
      replacement.push_back('\n');
  }

  for (const auto &candidate : candidates) {
    (void)widenMaterializedHeaderMacroState(
        materializedHeaderMacroStateBoundary(candidate.begin, candidate.end),
        StateMutationKind::MovedLater,
        llvm::formatv(
            "include-owned macro patch in {0} carries #define #{1} for "
            "'{2}' after replay interval [{3},{4})",
            headerPath, candidate.directive->id, candidate.name, newStart,
            newEnd)
            .str());
  }

  StabilizedMaterializedHeaderMacroPatch stabilized;
  stabilized.start = newStart;
  stabilized.end = newEnd;
  stabilized.replacement = std::move(replacement);
  stabilized.movedTransitions.assign(candidates.begin(), candidates.end());
  return stabilized;
}

} // namespace refold
} // namespace clang

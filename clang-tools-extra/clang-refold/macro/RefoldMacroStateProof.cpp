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

#include "model/RefoldPathIdentity.h"
#include "proof/RefoldOwnerStateProof.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "support/RefoldLog.h"
#include "support/StringUtils.h"

#include "llvm/ADT/DenseSet.h"
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
  if (directive.IsFunctionLikeDefine() && directive.name == macroName) {
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
  if (!directive.IsMacroStateDirective())
    return false;
  if (directive.name.empty())
    return unprovenObserves;
  return FirstMacroStateObservationOffsetInText(directive, directive.name,
                                                replacement)
      .has_value();
}

/// Return whether every recorded `#define` for `name` agrees on
/// function-likeness.
///
/// The observation mode for a name is a property of the definition live at the
/// replay position, not of the one directive being crossed.  When two recorded
/// definitions disagree, a bare NAME observes one and not the other, so
/// crossing a directive that selects between them is itself observable and the
/// shape-exact rule cannot decide the name.
static bool recordedDefinitionsAgreeOnShape(const RefoldModel &model,
                                            StringRef name, bool functionLike) {
  for (const RefoldModel::MacroDirective *directive :
       model.GetMacroDirectivesByName(name)) {
    if (!directive)
      return false;
    if (!directive->IsDefine())
      continue;
    if (directive->functionLike != functionLike)
      return false;
  }
  return true;
}

/// Return whether a replacement-list token can synthesize a name no replacement
/// list spells.
///
/// `#` and `##` build their result from operand text at expansion time, so a
/// walk over recorded spellings cannot see what comes out.  Both digraph
/// spellings are included because the producer records the token as written.
static bool replacementTokenCanSynthesizeName(StringRef spelling) {
  return spelling == "#" || spelling == "##" || spelling == "%:" ||
         spelling == "%:%:";
}

/// Return whether expanding a macro named `start` can produce a token spelling
/// one of `boundNames`.
///
/// A payload identifier that is itself a live macro does not stand for itself:
/// it expands to its replacement list, whose identifiers may be live in turn.
/// Nothing short of the transitive closure over the producer's recorded
/// replacement lists answers whether a bound definition is reached, and reached
/// is what makes the payload's side of the directive observable.
///
/// `start` itself is not tested against `boundNames`; spelling a bound name is
/// the direct observation the caller decides in that binding's own mode.  This
/// answers only what expanding the spelling can additionally produce.
///
/// `ParamRef` tokens stand for argument text rather than for a name of their
/// own.  Arguments are spelled in the payload, so the caller's identifier
/// inventory already seeds a walk from each of them, and following the
/// parameter here would add nothing.
///
/// A `#`/`##` operand reports reachable: it can build a bound name out of
/// pieces, and no recorded spelling shows the result.
static bool macroExpansionCanReachBoundName(const RefoldModel &model,
                                            StringRef start,
                                            ArrayRef<StringRef> boundNames) {
  SmallVector<StringRef, 8> worklist;
  SmallVector<StringRef, 16> visited;
  worklist.push_back(start);
  visited.push_back(start);

  while (!worklist.empty()) {
    const StringRef name = worklist.pop_back_val();
    for (const RefoldModel::MacroDirective *directive :
         model.GetMacroDirectivesByName(name)) {
      if (!directive)
        return true;
      if (!directive->IsDefine())
        continue;

      for (const RefoldModel::MacroReplacementToken &token :
           directive->replacementTokens) {
        if (token.kind != RefoldModel::MacroReplacementTokenKind::Literal)
          continue;
        if (replacementTokenCanSynthesizeName(token.spelling))
          return true;
        if (token.spelling.empty() ||
            !stringutils::isIdentStart(token.spelling.front())) {
          continue;
        }
        if (llvm::is_contained(boundNames, token.spelling))
          return true;
        if (!llvm::is_contained(visited, token.spelling)) {
          visited.push_back(token.spelling);
          worklist.push_back(token.spelling);
        }
      }
    }
  }
  return false;
}

bool RefoldMacroStateProof::PayloadObservesMacroStateBindings(
    ArrayRef<MacroStateBinding> bindings, StringRef payload,
    PayloadIdentifierExpansionPolicy policy) const {
  // A placement question is asked only because a directive was preserved, so an
  // empty list means the bindings were never recovered.
  if (bindings.empty())
    return true;
  if (payload.empty())
    return false;

  SmallVector<StringRef, 2> boundNames;
  for (const MacroStateBinding &binding : bindings) {
    if (binding.name.empty())
      return true;
    boundNames.push_back(binding.name);

    if (!binding.directive) {
      // No record of the definition's shape, so the conservative
      // identifier-token observation is the only one available.
      if (tokenText_.RawIdentifierAppearsInText(binding.name, payload))
        return true;
      continue;
    }

    if (FirstMacroStateObservationOffsetInText(*binding.directive, binding.name,
                                               payload)
            .has_value()) {
      return true;
    }
    // The shape-exact rule just cleared a spelling of this name.  It may do so
    // only when every recorded definition agrees on the mode it decided by.
    if (tokenText_.RawIdentifierAppearsInText(binding.name, payload) &&
        !recordedDefinitionsAgreeOnShape(model_, binding.name,
                                         binding.directive->functionLike)) {
      return true;
    }
  }

  // Reachability.  The producer records every `#define` the preprocessor saw,
  // so an identifier no record binds cannot be live and stands for itself; one
  // that is bound expands, and its expansion may name a bound macro the payload
  // never spells.
  //
  // A caller under `NeutralisedByLivenessAudit` has already said this leg is
  // not its question: every identifier that could expand there is separately
  // required to be unbound where the payload lands, so none of them expands and
  // none reaches a binding.  The leg is skipped whole rather than per
  // identifier, because "has a recorded definition" is precisely the condition
  // under which the walk can report anything at all -- an identifier with no
  // record terminates it immediately -- so filtering by it would leave nothing
  // behind while implying a distinction the audit does not make.
  if (policy == PayloadIdentifierExpansionPolicy::NeutralisedByLivenessAudit) {
    REFOLD_LOG_TRACE("macro/state",
                     "payload expansion-reachability deferred to the "
                     "macro-liveness audit for {0} binding(s)",
                     static_cast<uint64_t>(boundNames.size()));
    return false;
  }

  SmallVector<StringRef, 16> identifiers;
  tokenText_.CollectRawIdentifiersInText(payload, identifiers);
  for (StringRef identifier : identifiers) {
    if (macroExpansionCanReachBoundName(model_, identifier, boundNames)) {
      REFOLD_LOG_TRACE("macro/state",
                       "payload identifier '{0}' expands to a preserved "
                       "macro-state binding it does not spell",
                       identifier);
      return true;
    }
  }
  return false;
}

bool RefoldMacroStateProof::MacroDefinitionIsSelfReferentialIdentity(
    const RefoldModel::MacroDirective &directive) const {
  if (!directive.IsDefine() || directive.name.empty())
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

bool RefoldMacroStateProof::CertifiedPayloadReadsDefinition(
    const BPayloadTokenCertificate &payload,
    const RefoldModel::MacroDirective &definition, StringRef macroName) const {
  if (definition.functionLike &&
      CertifiedPayloadOpensCallOf(payload, macroName))
    return true;
  for (const auto &[begin, end] : payload.bTokenRanges)
    for (uint64_t index = begin; index < end && index < bToks_.size(); ++index)
      if (bToks_[index].kind == "raw_identifier" &&
          bToks_[index].spelling == macroName &&
          (!definition.functionLike ||
           (index + 1 < bToks_.size() && bToks_[index + 1].spelling == "(")))
        return true;
  return false;
}

/// Return the identifier \p toks places right before \p seam when the token at
/// \p seam is `(`: the name a seam there joins to that parenthesis.
static std::optional<StringRef> nameJoinedToParenAt(ArrayRef<PPTok> toks,
                                                    uint64_t seam) {
  if (seam == 0 || seam >= toks.size() ||
      toks[seam - 1].kind != "raw_identifier" || toks[seam].spelling != "(")
    return std::nullopt;
  return StringRef(toks[seam - 1].spelling);
}

bool RefoldMacroStateProof::CertifiedPayloadOpensCallOf(
    const BPayloadTokenCertificate &payload, StringRef macroName) const {
  return llvm::any_of(payload.bTokenRanges, [&](const auto &range) {
    return nameJoinedToParenAt(bToks_, range.first) == macroName;
  });
}

std::optional<StringRef> RefoldMacroStateProof::RecordedMacroNamedInHeaderBText(
    StringRef text,
    ArrayRef<const RefoldModel::MacroDirective *> carriedPastText) const {
  SmallVector<StringRef, 16> identifiers;
  tokenText_.CollectRawIdentifiersInText(text, identifiers);
  const DenseSet<StringRef> spelled(identifiers.begin(), identifiers.end());
  for (const RefoldModel::MacroDirective &directive :
       model_.GetMacroDirectives()) {
    if (!directive.IsDefine() || directive.name.empty() ||
        !spelled.contains(directive.name))
      continue;
    if (SelfReferentialDefinitionIsUnobservableInText(directive, directive.name,
                                                      text))
      continue;
    // A carried definition no longer precedes the text.  That leaves the name
    // unbound there only when it is the name's sole definition.
    if (llvm::is_contained(carriedPastText, &directive) &&
        llvm::count_if(model_.GetMacroDirectives(),
                       [&](const RefoldModel::MacroDirective &other) {
                         return other.IsDefine() &&
                                other.name == directive.name;
                       }) == 1)
      continue;
    return StringRef(directive.name);
  }
  return std::nullopt;
}

std::optional<StringRef>
RefoldMacroStateProof::RecordedFunctionLikeMacroCalledAt(ArrayRef<PPTok> toks,
                                                         uint64_t seam) const {
  std::optional<StringRef> name = nameJoinedToParenAt(toks, seam);
  if (!name || llvm::none_of(model_.GetMacroDirectivesByName(*name),
                             [](const RefoldModel::MacroDirective *directive) {
                               return directive->IsFunctionLikeDefine();
                             }))
    return std::nullopt;
  return name;
}

bool RefoldMacroStateProof::MovingDirectiveLineCouldOpenCall(
    const RefoldModel::MacroDirective &directive) const {
  const RefoldModel::IncludeItem *owner =
      directive.ownerIncludeId
          ? model_.GetIncludeById(*directive.ownerIncludeId)
          : nullptr;
  if (directive.ownerIncludeId && !owner)
    return true;
  // The first A token of the directive's file and inclusion spelled after its
  // line; the token before it in A is the one the line followed.
  std::optional<uint64_t> after;
  ArrayRef<RefoldModel::TokMapEntry> tokmap = model_.GetTokmap();
  for (size_t index = 0; index < tokmap.size(); ++index) {
    const RefoldModel::TokMapEntry &entry = tokmap[index];
    if (entry.b < directive.directiveLineE || (after && entry.pp >= *after))
      continue;
    if (owner ? entry.pp < owner->cover.begin || entry.pp >= owner->cover.end ||
                    !paths_.PathsEqual(entry.file, directive.sitePath)
              : !model_.TokmapEntryIsTUOwned(index))
      continue;
    after = entry.pp;
  }
  return after && RecordedFunctionLikeMacroCalledAt(aToks_, *after);
}

std::optional<const RefoldModel::MacroDirective *>
RefoldMacroStateProof::DefinitionBoundAtInvocation(uint64_t invocationId,
                                                   StringRef macroName) const {
  // `#pragma pop_macro` rebinds a name without a `#define` or `#undef`.
  for (const RefoldModel::PragmaDirective &pragma : model_.GetPragmas())
    if (pragma.id < invocationId && pragma.text.contains("pop_macro") &&
        pragma.text.contains(("\"" + macroName + "\"").str()))
      return std::nullopt;
  const RefoldModel::MacroDirective *last = nullptr;
  for (const RefoldModel::MacroDirective *directive :
       model_.GetMacroDirectivesByName(macroName))
    if (directive->id < invocationId && (!last || directive->id > last->id))
      last = directive;
  return last && last->IsDefine() ? last : nullptr;
}

bool RefoldMacroStateProof::InstanceRangeRecordsUndefObserver(
    StringRef path, uint64_t ownerIncludeId, StringRef macroName,
    uint64_t begin, uint64_t end) const {
  if (begin >= end)
    return false;
  const auto meetsRange = [&](uint64_t extentBegin, uint64_t extentEnd) {
    return extentBegin < end && begin < std::max(extentEnd, extentBegin + 1);
  };
  // A pragma record without an owner comes from a map too old to bind it to
  // one inclusion, so it is taken to be in every inclusion of its file.
  for (const RefoldModel::PragmaDirective &pragma : model_.GetPragmas()) {
    if (pragma.ownerIncludeId && *pragma.ownerIncludeId != ownerIncludeId)
      continue;
    if (paths_.PathsEqual(pragma.sitePath, path) &&
        meetsRange(pragma.siteB, pragma.siteE))
      return true;
  }
  for (const RefoldModel::MacroInvocation &invocation :
       model_.GetMacroInvocations()) {
    if (invocation.name != macroName ||
        invocation.ownerIncludeId != ownerIncludeId)
      continue;
    if (!invocation.useB || !invocation.useE ||
        meetsRange(*invocation.useB, *invocation.useE))
      return true;
  }
  return false;
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
        return recoverMacroStateDirectiveLineInterval(
            paths_, directive, headerPath, bytes,
            std::optional<uint64_t>(includeId));
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

  // Collect the definitions bound where the replacement is read that it would
  // observe.  A replacement wholly made of B is asked what its text names and
  // what its B tokens read, which covers a call the payload opens; one that
  // keeps preserved callsite spelling is asked only about its B tokens, since
  // that spelling may need the expansion it names.  Processing order fixes
  // the binding wherever the definition was spelled.  One in this header
  // instance may be carried past the replacement or bracketed around it; one
  // bound from anywhere else -- the translation unit, another header, another
  // inclusion of this one -- can only be bracketed.  An observed definition no
  // repair can address fails closed: repairing the others would leave its read
  // in place.
  const bool keepsPreservedSpelling =
      mp.bPayload && mp.bPayload->rest != BPayloadRest::None;
  SmallVector<MaterializedHeaderMacroStateDirectivePiece, 4> candidates;
  bool boundOutsideHeader = false;
  bool observedUnrepairable = false;
  for (const auto &directive : model_.GetMacroDirectives()) {
    if (!directive.IsDefine())
      continue;
    const bool observed =
        (!keepsPreservedSpelling && ReplacementObservesMacroStateDirective(
                                        directive, StringRef(mp.replacement),
                                        /*unprovenObserves=*/false)) ||
        (mp.bPayload && CertifiedPayloadReadsDefinition(*mp.bPayload, directive,
                                                        directive.name));
    if (!observed)
      continue;
    const std::optional<const RefoldModel::MacroDirective *> bound =
        DefinitionBoundAtInvocation(mp.invocationId, directive.name);
    if (bound && *bound != &directive)
      continue;
    std::optional<MaterializedHeaderMacroStateDirectivePiece> piece =
        materializedHeaderMacroStateDirectiveInterval(directive);
    if (!bound || (piece && materializedHeaderRangeOverlapsStagedEdit(
                                piece->begin, piece->end))) {
      observedUnrepairable = true;
      continue;
    }
    boundOutsideHeader = boundOutsideHeader || !piece;
    candidates.push_back(
        piece ? std::move(*piece)
              : MaterializedHeaderMacroStateDirectivePiece{
                    &directive, /*begin=*/0, /*end=*/0, directive.name});
  }

  if (candidates.empty() && !observedUnrepairable)
    return std::nullopt;
  if (observedUnrepairable) {
    (void)failMaterializedHeaderMacroState(
        materializedHeaderMacroStateBoundary(mp.invStart, mpEnd),
        StateMutationKind::Materialized,
        llvm::formatv("include-owned macro patch in {0} observes macro state "
                      "before [{1},{2}) that no repair can address",
                      headerPath, mp.invStart, mpEnd)
            .str());
    return StabilizedMaterializedHeaderMacroPatch{};
  }

  // A repair that rebinds a name over the whole replacement must leave the
  // replacement's bytes other than B payload reading what they read before.
  // A patch keeping its callsite re-spells the callsite's own source there;
  // an unproven rest counts as observing.  A patch with no certificate
  // realizes the callsite rather than keeping it.
  const auto replacementRestObserves =
      [&](const MaterializedHeaderMacroStateDirectivePiece &candidate) {
        if (!mp.bPayload || mp.bPayload->rest == BPayloadRest::None)
          return false;
        return mp.bPayload->rest == BPayloadRest::Unproven ||
               SourceChunkObservesMacroStateDirectiveWhenCrossed(
                   *candidate.directive, candidate.name,
                   bytes.slice(mp.invStart, mpEnd), bytes.drop_front(mpEnd)) ||
               InstanceRangeRecordsUndefObserver(
                   headerPath, includeId, candidate.name, mp.invStart, mpEnd);
      };

  // Emit carried directives in source order so the relative macro-state
  // transition order among multiple carried definitions is preserved.
  llvm::sort(candidates,
             [](const MaterializedHeaderMacroStateDirectivePiece &lhs,
                const MaterializedHeaderMacroStateDirectivePiece &rhs) {
               if (lhs.begin != rhs.begin)
                 return lhs.begin < rhs.begin;
               return lhs.directive->id < rhs.directive->id;
             });

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

  // Alternative repair, attempted only when a definition cannot be carried:
  // leave every definition exactly where it is, undefine the observed ones
  // immediately before the replacement, and restore them immediately after it
  // from the producer's directive text.
  //
  // The restore is what makes this self-contained.  Macro state after the
  // repaired interval is identical to the state before it, so no obligation
  // falls on the header suffix or on translation-unit source after the include
  // -- neither of which this proof can see.
  //
  // Two brackets implement it, and they trade different obligations.  The wider
  // one spans the replacement's whole physical line and asks that line's
  // preserved prefix and suffix to be indifferent to the name; the narrower one
  // spans only the replacement and instead asks that the bytes it reinterprets
  // are B-derived payload.  Neither subsumes the other, so both are attempted,
  // widest first.
  //
  // The wider bracket: preferred because it is the established shape, so every
  // input it already handled stays byte-identical.
  auto buildUndefRestoreAroundReplacementLine =
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
      // defined.  Neither may observe it, by spelling it or otherwise.
      if (SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *candidate.directive, candidate.name, sameLinePrefix,
              StringRef(mp.replacement)) ||
          InstanceRangeRecordsUndefObserver(
              headerPath, includeId, candidate.name, lineStart, mp.invStart) ||
          replacementRestObserves(candidate)) {
        return std::nullopt;
      }
      if (SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *candidate.directive, candidate.name, sameLineSuffix,
              StringRef()) ||
          InstanceRangeRecordsUndefObserver(
              headerPath, includeId, candidate.name, mpEnd, restoreEnd)) {
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

  // The narrower bracket, for a line the wider one cannot serve.  A line may
  // carry both B-derived payload and preserved source that *requires* the name
  // to expand -- `int patched(void) { return ID(4242) + VAL; }` edited to read
  // `VAL` inside the invocation -- where undefining across the whole line
  // breaks the preserved `+ VAL` and not undefining breaks the payload.
  // Bracketing only the replacement leaves every byte outside it in its
  // original macro state, so the surrounding source is asked for nothing at all
  // and the two readings of the name coexist on one line.
  auto buildUndefRestoreAroundBPayload =
      [&]() -> std::optional<StabilizedMaterializedHeaderMacroPatch> {
    // Only B-derived bytes may be reinterpreted under a rewritten macro
    // environment.  Without the partition certificate the replacement may hold
    // preserved callsite spelling that requires the name to expand -- an
    // args-only patch emitting `ID(VAL)` is exactly that -- so absence of the
    // certificate keeps this repair unavailable.
    if (!mp.bPayload || mp.bPayload->rest != BPayloadRest::None)
      return std::nullopt;

    // A directive inside the replacement may select an arm whose conditional
    // state is itself part of the owner proof, which is no longer a plain
    // token repair.  A B-derived payload carries no directive, so this excludes
    // only cases this repair was never meant to cover.
    if (tokenText_.TextContainsDirectiveLine(StringRef(mp.replacement)))
      return std::nullopt;

    // The synthesized directives are inserted at the replacement's own
    // boundaries, each on a fresh physical line.  Both boundaries are token
    // boundaries by construction, so the added newlines are ordinary
    // whitespace -- unless the replacement sits on a directive line, where
    // splitting the logical line would change what the directive says, or
    // inside a spliced logical line, where a new physical line ends the splice.
    const uint64_t lineStart = static_cast<uint64_t>(
        stringutils::lineStartOffset(bytes, static_cast<size_t>(mp.invStart)));
    if (lineStart > mp.invStart)
      return std::nullopt;
    if (lineStart > 0 &&
        stringutils::isLineSplice(bytes, static_cast<size_t>(lineStart) - 1))
      return std::nullopt;
    const uint64_t enclosingLineEnd = materializedHeaderLineEndAfter(mpEnd);
    if (enclosingLineEnd < mpEnd || enclosingLineEnd > bytes.size())
      return std::nullopt;
    if (tokenText_.TextContainsDirectiveLine(
            bytes.slice(lineStart, enclosingLineEnd)))
      return std::nullopt;

    if (materializedHeaderRangeOverlapsStagedEdit(mp.invStart, mpEnd))
      return std::nullopt;

    for (const auto &candidate : candidates) {
      // The definition must be complete before the synthesized `#undef`, or the
      // `#undef` would precede the `#define` it undoes.
      if (candidate.end > mp.invStart)
        return std::nullopt;
    }

    // Source outside `[mp.invStart, mpEnd)` is untouched and keeps the macro
    // state it originally had: the prefix is preprocessed before the `#undef`
    // and the suffix after the restore.  Neither is asked for anything, which
    // is what lets a line carry both readings of the name.
    std::string replacement;
    replacement.push_back('\n');
    for (const auto &candidate : candidates)
      replacement += (Twine("#undef ") + candidate.name + "\n").str();
    replacement += mp.replacement;
    if (replacement.empty() || replacement.back() != '\n')
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
          materializedHeaderMacroStateBoundary(mp.invStart, mpEnd),
          StateMutationKind::WidenedIntoClosure,
          llvm::formatv("include-owned macro patch in {0} undefines '{1}' "
                        "(#{2}) across B-derived replay payload [{3},{4}) and "
                        "restores it immediately after",
                        headerPath, candidate.name, candidate.directive->id,
                        mp.invStart, mpEnd)
              .str());
    }

    StabilizedMaterializedHeaderMacroPatch stabilized;
    stabilized.start = mp.invStart;
    stabilized.end = mpEnd;
    stabilized.replacement = std::move(replacement);
    return stabilized;
  };

  // Widen from the first carried directive through the completed invocation
  // line.  The suffix after the invocation is consumed and replayed before the
  // carried directives, which keeps the directives line-started after repair.
  //
  // The replacement payload will be emitted before each carried definition,
  // and the original line suffix after the macro invocation must also remain
  // before those definitions so the directives still start on real directive
  // lines.  Therefore every byte crossed by each carried definition must be
  // proven non-observing for that definition.  Otherwise the macro-state move
  // would change how preserved header source preprocesses, and the
  // replacement is bracketed instead.  A definition bound outside this header
  // instance has no line here to carry at all.
  const uint64_t newStart =
      boundOutsideHeader ? mp.invStart : candidates.front().begin;
  const uint64_t newEnd = materializedHeaderLineEndAfter(mpEnd);
  bool carryAvailable =
      !boundOutsideHeader && newStart <= mp.invStart && newEnd >= mpEnd &&
      newEnd <= bytes.size() &&
      !materializedHeaderRangeOverlapsStagedEdit(newStart, newEnd);
  for (const auto &candidate : candidates) {
    if (!carryAvailable)
      break;
    std::string crossed;
    appendCrossedSourceExcludingCarried(crossed, candidate.end, mp.invStart);
    appendCrossedSourceExcludingCarried(crossed, mpEnd, newEnd);
    StringRef following = bytes.slice(mp.invStart, mpEnd);
    carryAvailable =
        !SourceChunkObservesMacroStateDirectiveWhenCrossed(
            *candidate.directive, candidate.name, StringRef(crossed),
            following) &&
        !InstanceRangeRecordsUndefObserver(headerPath, includeId,
                                           candidate.name, candidate.end,
                                           mp.invStart) &&
        !InstanceRangeRecordsUndefObserver(headerPath, includeId,
                                           candidate.name, mpEnd, newEnd) &&
        !replacementRestObserves(candidate) &&
        !MovingDirectiveLineCouldOpenCall(*candidate.directive);
  }
  if (!carryAvailable) {
    if (std::optional<StabilizedMaterializedHeaderMacroPatch> undefRestore =
            buildUndefRestoreAroundReplacementLine())
      return undefRestore;
    if (std::optional<StabilizedMaterializedHeaderMacroPatch> undefRestore =
            buildUndefRestoreAroundBPayload())
      return undefRestore;
    (void)failMaterializedHeaderMacroState(
        materializedHeaderMacroStateBoundary(mp.invStart, mpEnd),
        StateMutationKind::MovedLater,
        llvm::formatv("include-owned macro patch in {0} can neither carry the "
                      "definitions it observes past [{1},{2}) nor undefine "
                      "and restore them around it",
                      headerPath, mp.invStart, mpEnd)
            .str());
    return StabilizedMaterializedHeaderMacroPatch{};
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

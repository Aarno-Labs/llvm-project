//===--- RefoldStructuralGapCrossingProof.cpp ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldStructuralGapCrossingProof.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "proof/RefoldPragmaTaxonomy.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "util/StringUtils.h"

#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Build the rejected answer for one structure.
GapCrossingEvidence rejectStructure(
    const PreprocessingStructureInterval &interval,
    GapCrossingRejection rejection) {
  GapCrossingEvidence evidence;
  evidence.structureKind = interval.kind;
  evidence.structureBegin = interval.begin;
  evidence.structureEnd = interval.end;
  evidence.rejection = rejection;
  return evidence;
}

/// Build the proven answer for one structure.
GapCrossingEvidence admitStructure(
    const PreprocessingStructureInterval &interval, GapCrossingProofKind kind) {
  GapCrossingEvidence evidence;
  evidence.structureKind = interval.kind;
  evidence.structureBegin = interval.begin;
  evidence.structureEnd = interval.end;
  evidence.proof = kind;
  evidence.rejection = GapCrossingRejection::None;
  return evidence;
}

/// Return whether a pragma's state effect is one the preprocessor consumes.
///
/// Equivalence requires that moving the payload across the directive change no
/// token order, which holds only when the preprocessor consumes the directive
/// and emits nothing for it.  A pragma that is printed instead -- `message`,
/// `warning`, `error`, `GCC diagnostic` -- is spelled into both streams, so the
/// payload's side of it is token order and is fixed by the alignment, not free
/// to choose.
bool pragmaEffectIsConsumed(PragmaStateEffect effect) {
  switch (effect) {
  case PragmaStateEffect::IncludeOnce:
  case PragmaStateEffect::MacroStateStack:
  case PragmaStateEffect::PoisonIdentifiers:
  case PragmaStateEffect::SystemHeader:
    return true;
  case PragmaStateEffect::Unknown:
  case PragmaStateEffect::NoState:
  case PragmaStateEffect::DiagnosticState:
    return false;
  }
  llvm_unreachable("invalid pragma state effect");
}

/// Return whether a structure kind is a conditional-control directive.
bool isConditionalControlKind(PreprocessingStructureKind kind) {
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

} // namespace

StringRef toString(GapCrossingProofKind kind) {
  switch (kind) {
  case GapCrossingProofKind::MacroStateDirectiveUnobserved:
    return "MacroStateDirectiveUnobserved";
  case GapCrossingProofKind::ConsumedPragmaUnobserved:
    return "ConsumedPragmaUnobserved";
  case GapCrossingProofKind::StatelessDiagnosticDirective:
    return "StatelessDiagnosticDirective";
  case GapCrossingProofKind::ConditionalControlSelectedArm:
    return "ConditionalControlSelectedArm";
  case GapCrossingProofKind::StatelessIncludeInstance:
    return "StatelessIncludeInstance";
  case GapCrossingProofKind::LineControlUnobserved:
    return "LineControlUnobserved";
  }
  llvm_unreachable("invalid gap-crossing proof kind");
}

StringRef toString(GapCrossingRejection rejection) {
  switch (rejection) {
  case GapCrossingRejection::None:
    return "None";
  case GapCrossingRejection::NoRuleForStructureKind:
    return "NoRuleForStructureKind";
  case GapCrossingRejection::NoProducerRecord:
    return "NoProducerRecord";
  case GapCrossingRejection::PayloadCarriesDirective:
    return "PayloadCarriesDirective";
  case GapCrossingRejection::PayloadObservesState:
    return "PayloadObservesState";
  case GapCrossingRejection::PragmaNotConsumed:
    return "PragmaNotConsumed";
  case GapCrossingRejection::IncludeInstanceCarriesState:
    return "IncludeInstanceCarriesState";
  case GapCrossingRejection::ArmNotSelected:
    return "ArmNotSelected";
  }
  llvm_unreachable("invalid gap-crossing rejection");
}

bool RefoldStructuralGapCrossingProver::CommittedTextCarriesDirective(
    const Query &query) const {
  if (deps_.tokenText.TextContainsDirectiveLine(query.committedSuffix))
    return true;
  // `_Pragma`/`__pragma` mutate preprocessor state from a normal-token
  // position, so the directive-line inventory does not see them.
  return deps_.tokenText.RawIdentifierAppearsInText("_Pragma",
                                                    query.committedSuffix) ||
         deps_.tokenText.RawIdentifierAppearsInText("__pragma",
                                                    query.committedSuffix);
}

const RefoldModel::PragmaDirective *
RefoldStructuralGapCrossingProver::ProducerPragmaFor(
    const PreprocessingStructureInterval &interval) const {
  if (interval.modelKind != PreprocessingStructureModelKind::PragmaDirective ||
      !interval.modelItemId) {
    return nullptr;
  }
  const RefoldModel::PragmaDirective *found = nullptr;
  for (const RefoldModel::PragmaDirective &pragma : deps_.model.GetPragmas()) {
    if (pragma.id != *interval.modelItemId)
      continue;
    if (found)
      return nullptr;
    found = &pragma;
  }
  return found;
}

const RefoldModel::LineControlEvent *
RefoldStructuralGapCrossingProver::ProducerLineControlFor(
    const PreprocessingStructureInterval &interval) const {
  if (interval.modelKind != PreprocessingStructureModelKind::LineControlEvent ||
      !interval.modelItemId) {
    return nullptr;
  }
  const RefoldModel::LineControlEvent *found = nullptr;
  for (const RefoldModel::LineControlEvent &event :
       deps_.model.GetLineControls()) {
    if (event.id != *interval.modelItemId)
      continue;
    if (found)
      return nullptr;
    found = &event;
  }
  return found;
}

GapCrossingEvidence RefoldStructuralGapCrossingProver::ProvePragma(
    const PreprocessingStructureInterval &interval,
    const Query &query) const {
  // The producer's recorded pragma text is what the preprocessor actually saw,
  // including a `_Pragma` operand assembled by macro expansion.  Classifying it
  // rather than the physical bytes is therefore both more exact and the only
  // form of the question that stays answerable for the operator spelling and
  // for a gap inside an included header.
  const RefoldModel::PragmaDirective *pragma = ProducerPragmaFor(interval);
  if (!pragma || pragma->text.empty())
    return rejectStructure(interval, GapCrossingRejection::NoProducerRecord);

  const PragmaClassification classification =
      classifyPragmaDirective(pragma->text);
  if (!pragmaEffectIsConsumed(classification.effect))
    return rejectStructure(interval, GapCrossingRejection::PragmaNotConsumed);

  // `push_macro`/`pop_macro` change the definition bound to one macro name,
  // which is the state a `#define` changes and the question the macro-state
  // proof answers.  The classification records the name but nothing about
  // either definition it swaps between, so the binding carries no directive and
  // takes the conservative identifier-token observation mode.  Answering it here
  // rather than inside the taxonomy is what keeps that classification a pure
  // function of text.
  MacroStateObservationAnswer macroState =
      MacroStateObservationAnswer::Unproven;
  if (classification.effect == PragmaStateEffect::MacroStateStack) {
    SmallVector<MacroStateBinding, 2> pushedBindings;
    for (StringRef pushedName : classification.namedIdentifiers)
      pushedBindings.push_back(MacroStateBinding{pushedName, nullptr});
    macroState = deps_.macroStateProof.PayloadObservesMacroStateBindings(
                     pushedBindings, query.payload)
                     ? MacroStateObservationAnswer::Observed
                     : MacroStateObservationAnswer::Unobserved;
  }

  if (payloadObservesPragmaState(classification, query.payload, deps_.lexLang,
                                 macroState)) {
    return rejectStructure(interval,
                           GapCrossingRejection::PayloadObservesState);
  }
  return admitStructure(interval,
                        GapCrossingProofKind::ConsumedPragmaUnobserved);
}

GapCrossingEvidence
RefoldStructuralGapCrossingProver::ProveConditionalControl(
    const PreprocessingStructureInterval &interval,
    const Query &query) const {
  // The directive itself is preserved in place, so what changes is only where
  // the payload lands relative to it: inside the arm the control opens, or
  // before the control.  That is sound exactly when the payload's new home is
  // reached, and the producer records reachability as arm selection.
  if (interval.modelKind !=
          PreprocessingStructureModelKind::ConditionalDirective ||
      !interval.conditionalGroupId ||
      !deps_.model.GetCondGroupById(*interval.conditionalGroupId)) {
    return rejectStructure(interval, GapCrossingRejection::NoProducerRecord);
  }

  // A payload that can introduce a directive is not answered by reachability:
  // an `#endif` or `#if` inside it would change the conditional nesting the
  // preserved control belongs to.
  if (CommittedTextCarriesDirective(query)) {
    return rejectStructure(interval,
                           GapCrossingRejection::PayloadCarriesDirective);
  }

  // No committed arm means the payload lands outside every conditional arm,
  // which the surrounding translation unit always reaches.  A committed arm
  // must be one the producer selected.  The run carrying that arm contributed A
  // tokens, so a producer record saying otherwise is an inconsistency, and
  // checking it is what keeps the reachability argument evidence-backed rather
  // than assumed.
  if (!query.committedArmId)
    return admitStructure(interval,
                          GapCrossingProofKind::ConditionalControlSelectedArm);

  std::optional<RefoldModel::ArmRef> armRef =
      deps_.model.GetArmRefById(*query.committedArmId);
  if (!armRef || !armRef->arm)
    return rejectStructure(interval, GapCrossingRejection::NoProducerRecord);
  if (!armRef->arm->selected)
    return rejectStructure(interval, GapCrossingRejection::ArmNotSelected);
  return admitStructure(interval,
                        GapCrossingProofKind::ConditionalControlSelectedArm);
}

GapCrossingEvidence RefoldStructuralGapCrossingProver::ProveInclude(
    const PreprocessingStructureInterval &interval,
    const Query &query) const {
  if (interval.modelKind !=
          PreprocessingStructureModelKind::IncludeDirective ||
      !interval.modelItemId) {
    return rejectStructure(interval, GapCrossingRejection::NoProducerRecord);
  }
  const RefoldModel::IncludeItem *include =
      deps_.model.GetIncludeById(*interval.modelItemId);
  if (!include)
    return rejectStructure(interval, GapCrossingRejection::NoProducerRecord);

  // An include is text plus a preprocessor-state transition.  This rule answers
  // only the degenerate instance that is neither: it contributes no A token,
  // and the producer attributed no directive of any kind to it.  With no macro
  // definition, pragma, line control, conditional group or nested include
  // recorded under the instance, entering it changed nothing a payload on
  // either side could observe.  Every other include keeps failing closed.
  if (include->cover.IsValid() || !include->spans.empty())
    return rejectStructure(interval,
                           GapCrossingRejection::IncludeInstanceCarriesState);

  const uint64_t includeId = include->id;
  for (const RefoldModel::MacroDirective &directive :
       deps_.model.GetMacroDirectives()) {
    if (directive.ownerIncludeId == includeId)
      return rejectStructure(interval,
                             GapCrossingRejection::IncludeInstanceCarriesState);
  }
  for (const RefoldModel::PragmaDirective &pragma : deps_.model.GetPragmas()) {
    if (pragma.ownerIncludeId == includeId)
      return rejectStructure(interval,
                             GapCrossingRejection::IncludeInstanceCarriesState);
  }
  for (const RefoldModel::LineControlEvent &event :
       deps_.model.GetLineControls()) {
    if (event.ownerIncludeId == includeId)
      return rejectStructure(interval,
                             GapCrossingRejection::IncludeInstanceCarriesState);
  }
  for (const RefoldModel::CondGroup &group : deps_.model.GetConds()) {
    if (group.parentIncludeId == includeId)
      return rejectStructure(interval,
                             GapCrossingRejection::IncludeInstanceCarriesState);
  }
  for (const RefoldModel::IncludeItem &nested : deps_.model.GetIncludes()) {
    if (nested.parent == includeId)
      return rejectStructure(interval,
                             GapCrossingRejection::IncludeInstanceCarriesState);
  }

  // With the instance proven stateless, the one remaining way the placement
  // could matter is a payload that performs inclusion itself, since the include
  // guard and once-state it would consult are established at this position.
  if (CommittedTextCarriesDirective(query)) {
    return rejectStructure(interval,
                           GapCrossingRejection::PayloadCarriesDirective);
  }
  return admitStructure(interval,
                        GapCrossingProofKind::StatelessIncludeInstance);
}

GapCrossingEvidence RefoldStructuralGapCrossingProver::ProveLineControl(
    const PreprocessingStructureInterval &interval,
    const Query &query) const {
  if (!ProducerLineControlFor(interval))
    return rejectStructure(interval, GapCrossingRejection::NoProducerRecord);

  if (CommittedTextCarriesDirective(query)) {
    return rejectStructure(interval,
                           GapCrossingRejection::PayloadCarriesDirective);
  }

  // A `#line` shifts the logical position every later observer reports, so the
  // payload's side of one is fixed as soon as the payload contains an observer.
  // Text that contains none observes nothing, which is the narrow admissible
  // case: the question is undecidable in general only because the observer may
  // be reached through a macro the payload never spells, and the macro-state
  // proof already closes over recorded replacement lists to answer exactly that.
  SmallVector<MacroStateBinding, 4> observerBindings;
  for (StringRef observer :
       {StringRef("__LINE__"), StringRef("__FILE__"),
        StringRef("__FILE_NAME__"), StringRef("__BASE_FILE__")}) {
    assert(stringutils::isLineDirectiveSensitiveBuiltin(observer) &&
           "line-control observer set must match the shared predicate");
    observerBindings.push_back(MacroStateBinding{observer, nullptr});
  }
  if (deps_.macroStateProof.PayloadObservesMacroStateBindings(
          observerBindings, query.committedSuffix)) {
    return rejectStructure(interval,
                           GapCrossingRejection::PayloadObservesState);
  }
  return admitStructure(interval, GapCrossingProofKind::LineControlUnobserved);
}

GapCrossingEvidence
RefoldStructuralGapCrossingProver::ProveDiagnosticDirective(
    const PreprocessingStructureInterval &interval,
    const Query &query) const {
  if (CommittedTextCarriesDirective(query)) {
    return rejectStructure(interval,
                           GapCrossingRejection::PayloadCarriesDirective);
  }

  // A `#warning` emits a diagnostic, mutates no preprocessor state and produces
  // no token, so there is nothing for either placement to observe.  This holds
  // for every payload, not merely for one whose observers were checked.
  if (interval.kind == PreprocessingStructureKind::WarningDirective) {
    return admitStructure(interval,
                          GapCrossingProofKind::StatelessDiagnosticDirective);
  }

  // A reached `#error` has already made the translation unit ill-formed, and
  // nothing here separates "reached" from "skipped" on its own.  The producer's
  // arm selection does: an `#error` the producer recorded inside an arm it did
  // not select was never executed, and preserving the gap's bytes in place
  // leaves it unexecuted.
  if (!interval.ownerConditionalArmId)
    return rejectStructure(interval, GapCrossingRejection::NoProducerRecord);
  std::optional<RefoldModel::ArmRef> armRef =
      deps_.model.GetArmRefById(*interval.ownerConditionalArmId);
  if (!armRef || !armRef->arm)
    return rejectStructure(interval, GapCrossingRejection::NoProducerRecord);
  if (armRef->arm->selected)
    return rejectStructure(interval, GapCrossingRejection::PayloadObservesState);
  return admitStructure(interval,
                        GapCrossingProofKind::StatelessDiagnosticDirective);
}

GapCrossingEvidence RefoldStructuralGapCrossingProver::ProveOneStructure(
    const PreprocessingStructureInterval &interval,
    const Query &query) const {
  if (isConditionalControlKind(interval.kind))
    return ProveConditionalControl(interval, query);

  switch (interval.kind) {
  case PreprocessingStructureKind::Pragma:
  case PreprocessingStructureKind::PragmaOperator:
    return ProvePragma(interval, query);

  case PreprocessingStructureKind::Include:
  case PreprocessingStructureKind::IncludeNext:
    return ProveInclude(interval, query);

  case PreprocessingStructureKind::LineControl:
    return ProveLineControl(interval, query);

  case PreprocessingStructureKind::ErrorDirective:
  case PreprocessingStructureKind::WarningDirective:
    return ProveDiagnosticDirective(interval, query);

  case PreprocessingStructureKind::MacroDefine:
  case PreprocessingStructureKind::MacroUndef:
    llvm_unreachable("macro-state directives are answered as one set");

  case PreprocessingStructureKind::Import:
  case PreprocessingStructureKind::OtherDirective:
    // `#import` is a Clang extension in neither C nor C++, and an unrecognized
    // directive may do anything.  Both stay unclassified by design.
    return rejectStructure(interval,
                           GapCrossingRejection::NoRuleForStructureKind);

  default:
    break;
  }
  return rejectStructure(interval,
                         GapCrossingRejection::NoRuleForStructureKind);
}

GapCrossingProof
RefoldStructuralGapCrossingProver::Prove(const Query &query) const {
  GapCrossingProof proof;

  // Recover the complete macro-state binding set first.  The macro-state proof
  // asks a reachable-set question over every bound name at once: an identifier
  // in the payload may expand to a name bound by one directive in the gap while
  // spelling another, so answering the directives one at a time would ask a
  // weaker question than the one that must hold.
  SmallVector<const PreprocessingStructureInterval *, 2> macroStateIntervals;
  for (const PreprocessingStructureInterval *interval : query.structures) {
    if (!interval)
      continue;
    if (interval->kind != PreprocessingStructureKind::MacroDefine &&
        interval->kind != PreprocessingStructureKind::MacroUndef) {
      continue;
    }
    proof.carriesMacroStateDirective = true;
    macroStateIntervals.push_back(interval);

    const RefoldModel::MacroDirective *directive =
        interval->modelKind ==
                    PreprocessingStructureModelKind::MacroDirective &&
                interval->modelItemId
            ? deps_.model.GetMacroDirectiveById(*interval->modelItemId)
            : nullptr;
    if (directive && !directive->name.empty())
      proof.macroBindings.push_back(
          MacroStateBinding{directive->name, directive});
    else
      proof.macroBindingsComplete = false;
  }

  // The macro-state answer is one verdict shared by every `#define`/`#undef` in
  // the gap.  It is asked of the committed suffix rather than of the ambiguous
  // payload alone: every byte from the committed boundary to the end of the
  // hunk lands after the directive, so the wider question subsumes the exact
  // placement obligation.
  const bool macroStateUnobserved =
      proof.macroBindingsComplete && !proof.macroBindings.empty() &&
      !deps_.macroStateProof.PayloadObservesMacroStateBindings(
          proof.macroBindings, query.committedSuffix);

  for (const PreprocessingStructureInterval *interval : query.structures) {
    if (!interval) {
      // A missing interval is not evidence of an empty gap; fail closed with a
      // structure whose kind nothing classifies.
      GapCrossingEvidence evidence;
      evidence.rejection = GapCrossingRejection::NoProducerRecord;
      proof.structures.push_back(evidence);
      continue;
    }

    if (interval->kind == PreprocessingStructureKind::MacroDefine ||
        interval->kind == PreprocessingStructureKind::MacroUndef) {
      proof.structures.push_back(
          macroStateUnobserved
              ? admitStructure(
                    *interval,
                    GapCrossingProofKind::MacroStateDirectiveUnobserved)
              : rejectStructure(*interval,
                                proof.macroBindingsComplete
                                    ? GapCrossingRejection::PayloadObservesState
                                    : GapCrossingRejection::NoProducerRecord));
      continue;
    }

    proof.structures.push_back(ProveOneStructure(*interval, query));
  }

  if (inTraceMode()) {
    for (const GapCrossingEvidence &evidence : proof.structures) {
      REFOLD_LOG_TRACE(
          "tiling/gap-crossing",
          "structure={0} source=[{1},{2}) crossable={3} proof={4} reason={5}",
          toString(evidence.structureKind), evidence.structureBegin,
          evidence.structureEnd, evidence.Proven(),
          evidence.Proven() ? toString(*evidence.proof) : StringRef("none"),
          toString(evidence.rejection));
    }
  }
  return proof;
}

} // namespace refold
} // namespace clang

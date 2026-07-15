//===--- RefoldWitnessEquivalenceKeyBuilder.cpp -----------------*- C++ -*-===//
//
// Implementation of the witness equivalence-key builder.  See the header for
// the architectural contract; the helpers in this translation unit are the
// per-dimension key-construction primitives used to render deterministic
// witness equivalence keys.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldWitnessEquivalenceKeyBuilder.h"

#include "proof/RefoldAcceptedResultPredicates.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldWitnessClassifier.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldWitnessEquivalenceKeyBuilder::RefoldWitnessEquivalenceKeyBuilder(
    Dependencies deps)
    : deps_(std::move(deps)) {}

// ===========================================================================
// BuildWitnessEquivalenceKey helpers.
//
// These translation-unit-local helpers keep witness key construction as a flat
// sequence of named rendering and canonicalization decisions.  They remain
// private implementation details of RefoldWitnessEquivalenceKeyBuilder rather
// than part of the public proof-service surface.
// ===========================================================================
namespace {

// --- pure formatters -------------------------------------------------------

/// Render an optional unsigned 64-bit value as decimal or the literal "none".
std::string formatOptionalU64(std::optional<uint64_t> value) {
  return value ? llvm::formatv("{0}", *value).str() : std::string("none");
}

/// Render an optional unsigned 32-bit value as decimal or the literal "none".
std::string formatOptionalU32(std::optional<uint32_t> value) {
  return value ? llvm::formatv("{0}", *value).str() : std::string("none");
}

/// Render an optional std::string verbatim or the literal "none".
std::string formatOptionalString(const std::optional<std::string> &value) {
  return value ? *value : std::string("none");
}

// --- identity-record appenders --------------------------------------------
//
// Each appender writes a single semicolon-terminated `label{...}` clause into
// `os`.  The output is consumed by the witness-equivalence key as part of the
// deterministic suffix-state / observer / counter signature; keeping the
// formatters here means every key dimension that references one of these
// records uses an identical textual shape.

/// Append a macro identity record: name, define/undef directive ids, arity,
/// variadic flag, and replacement-token hash.
void appendMacroIdentity(llvm::raw_ostream &os, llvm::StringRef label,
                         const MacroStateIdentity &identity) {
  os << label << "{name=" << identity.macroName
     << ":def=" << formatOptionalU64(identity.definitionDirectiveId)
     << ":undef=" << formatOptionalU64(identity.undefDirectiveId)
     << ":function_like=" << (identity.functionLike ? 1 : 0)
     << ":arity=" << formatOptionalU32(identity.arity)
     << ":variadic=" << (identity.variadic ? 1 : 0) << ":replacement="
     << RefoldWitnessTrace::FormatWitnessTraceHash(
            identity.replacementTokenHash)
     << "};";
}

/// Append a macro observation record (kind + identity).
void appendMacroObservation(llvm::raw_ostream &os, llvm::StringRef label,
                            const MacroStateObservation &observation) {
  os << label << "{kind=" << toString(observation.kind) << ':';
  appendMacroIdentity(os, "identity=", observation.identity);
  os << "};";
}

/// Append every macro observation/requirement bucket carried by `facts` under
/// the shared prefix `bucket`.
void appendMacroObservationBucket(llvm::raw_ostream &os, llvm::StringRef bucket,
                                  const OwnerStateFacts &facts) {
  for (const MacroStateIdentity &identity : facts.macroRequirements)
    appendMacroIdentity(os, llvm::formatv("{0}.requirement=", bucket).str(),
                        identity);
  for (const MacroStateObservation &observation :
       facts.macroExpansionObservations)
    appendMacroObservation(os, llvm::formatv("{0}.expansion=", bucket).str(),
                           observation);
  for (const MacroStateObservation &observation :
       facts.definedOperatorObservations)
    appendMacroObservation(os, llvm::formatv("{0}.defined=", bucket).str(),
                           observation);
  for (const MacroStateObservation &observation :
       facts.conditionalMacroObservations)
    appendMacroObservation(os, llvm::formatv("{0}.conditional=", bucket).str(),
                           observation);
}

/// Append an include identity record: directive kind, site, target, resolved
/// path, angled-ness, parent include id, and materialization flag.
void appendIncludeIdentity(llvm::raw_ostream &os, llvm::StringRef label,
                           const IncludeStateIdentity &identity) {
  os << label << "{id=" << identity.includeId
     << ":kind=" << identity.directiveKind << ":site=" << identity.sitePath
     << '[' << identity.siteBegin << ',' << identity.siteEnd << ")"
     << ":target=" << identity.target
     << ":resolved=" << formatOptionalString(identity.resolvedPath)
     << ":angled=" << (identity.angled ? 1 : 0)
     << ":parent=" << formatOptionalU64(identity.parentIncludeId)
     << ":tokens=" << (identity.hasTokenMaterialization ? 1 : 0) << "};";
}

/// Append an include-guard identity record.
void appendIncludeGuardIdentity(llvm::raw_ostream &os, llvm::StringRef label,
                                const IncludeGuardStateIdentity &identity) {
  os << label << "{kind=" << toString(identity.kind)
     << ":include=" << identity.includeId << ":header=" << identity.headerPath
     << ":guard=" << formatOptionalString(identity.guardMacroName)
     << ":parent=" << formatOptionalU64(identity.parentIncludeId)
     << ":producer=" << (identity.producerProvenGuard ? 1 : 0) << "};";
}

/// Append a line-control identity record (file, site, logical line/file
/// after, owner include, operand provenance).
void appendLineControlIdentity(llvm::raw_ostream &os, llvm::StringRef label,
                               const LineControlStateIdentity &identity) {
  os << label << "{id=" << identity.eventId << ":file=" << identity.physicalFile
     << ":site=[" << formatOptionalU64(identity.siteBegin) << ','
     << formatOptionalU64(identity.siteEnd) << ')'
     << ":active=" << (identity.active ? 1 : 0)
     << ":producer=" << (identity.producerProven ? 1 : 0)
     << ":line_after=" << identity.logicalLineAfter
     << ":file_after=" << identity.logicalFileAfter
     << ":owner_include=" << formatOptionalU64(identity.ownerIncludeId)
     << ":operands=" << toString(identity.operandProvenance) << "};";
}

/// Append a builtin-location observation record.
void appendBuiltinLocationObservation(
    llvm::raw_ostream &os, llvm::StringRef label,
    const BuiltinLocationObservation &observation) {
  os << label << "{kind=" << toString(observation.kind)
     << ":owner_include=" << formatOptionalU64(observation.ownerIncludeId)
     << ":source=[" << formatOptionalU64(observation.sourceBegin) << ','
     << formatOptionalU64(observation.sourceEnd) << ')' << ":atok=["
     << formatOptionalU64(observation.aTokenBegin) << ','
     << formatOptionalU64(observation.aTokenEnd) << ")};";
}

/// Append a counter event identity record.
void appendCounterIdentity(llvm::raw_ostream &os, llvm::StringRef label,
                           const CounterEventIdentity &identity) {
  os << label << "{macro=" << identity.macroInvocationId
     << ":ordinal=" << identity.occurrenceOrdinal
     << ":owner_include=" << formatOptionalU64(identity.ownerIncludeId)
     << ":caller=" << formatOptionalU64(identity.callerMacroId) << ":atok=["
     << identity.aTokenBegin << ',' << identity.aTokenEnd << ')'
     << ":site=" << identity.expansionSiteFile << '['
     << formatOptionalU64(identity.expansionSiteBegin) << ','
     << formatOptionalU64(identity.expansionSiteEnd) << ')'
     << ":avalue=" << identity.aValue << ":bvalue="
     << (identity.expectedBValue ? *identity.expectedBValue
                                 : std::string("none"))
     << ":literal=" << (identity.canStabilizeByLiteralization ? 1 : 0)
     << ":materialize=" << (identity.canStabilizeByMaterialization ? 1 : 0)
     << "};";
}

/// Append a pragma identity record.
void appendPragmaIdentity(llvm::raw_ostream &os, llvm::StringRef label,
                          const PragmaStateIdentity &identity) {
  os << label << "{id=" << identity.pragmaId << ":site=" << identity.sitePath
     << '[' << identity.siteBegin << ',' << identity.siteEnd << ')'
     << ":owner_include=" << formatOptionalU64(identity.ownerIncludeId)
     << ":class=" << toString(identity.classification) << ":fingerprint="
     << RefoldWitnessTrace::FormatWitnessTraceHash(
            identity.directiveFingerprint)
     << "};";
}

/// Append a conditional identity record (role, group, arm, parent, selection,
/// producer-truth, reverse-solved flag).
void appendConditionalIdentity(llvm::raw_ostream &os, llvm::StringRef label,
                               const ConditionalStateIdentity &identity) {
  os << label << "{role=" << toString(identity.role)
     << ":group=" << identity.groupId
     << ":arm=" << formatOptionalU64(identity.armId)
     << ":file=" << identity.file << '[' << identity.groupBegin << ','
     << identity.groupEnd << ')'
     << ":parent_arm=" << formatOptionalU64(identity.parentArmId)
     << ":parent_include=" << formatOptionalU64(identity.parentIncludeId)
     << ":arm_kind=" << identity.armKind
     << ":cond=" << formatOptionalString(identity.conditionText)
     << ":selected=" << (identity.selected ? 1 : 0) << ":atok=["
     << formatOptionalU64(identity.aTokenBegin) << ','
     << formatOptionalU64(identity.aTokenEnd) << ')'
     << ":producer_truth=" << (identity.conditionTruthProducerProven ? 1 : 0)
     << ":reverse_solved=" << (identity.reverseSolvedDirectiveRequired ? 1 : 0)
     << "};";
}

/// Append a missing-state-fact record.
void appendMissingFact(llvm::raw_ostream &os, llvm::StringRef label,
                       const MissingStateFact &fact) {
  os << label << "{kind=" << toString(fact.kind)
     << ":detail=" << RefoldWitnessTrace::FormatWitnessTraceHash(fact.detail)
     << "};";
}

/// Append every identity bucket carried by `facts` under the shared prefix
/// `bucket` (macros, line-control, builtins, counters, pragmas, includes,
/// include-guards, conditionals, and missing facts).
void appendFullStateFacts(llvm::raw_ostream &os, llvm::StringRef bucket,
                          const OwnerStateFacts &facts) {
  for (const MacroStateIdentity &identity : facts.macroDefinitions)
    appendMacroIdentity(os, llvm::formatv("{0}.macro_define=", bucket).str(),
                        identity);
  for (const MacroStateIdentity &identity : facts.macroUndefinitions)
    appendMacroIdentity(os, llvm::formatv("{0}.macro_undef=", bucket).str(),
                        identity);
  appendMacroObservationBucket(os, bucket, facts);
  for (const LineControlStateIdentity &identity : facts.lineControlEvents)
    appendLineControlIdentity(
        os, llvm::formatv("{0}.line_control=", bucket).str(), identity);
  for (const BuiltinLocationObservation &observation :
       facts.builtinLocationObservations)
    appendBuiltinLocationObservation(
        os, llvm::formatv("{0}.builtin=", bucket).str(), observation);
  for (const CounterEventIdentity &identity : facts.counterEvents)
    appendCounterIdentity(os, llvm::formatv("{0}.counter=", bucket).str(),
                          identity);
  for (const PragmaStateIdentity &identity : facts.pragmaStateEvents)
    appendPragmaIdentity(os, llvm::formatv("{0}.pragma=", bucket).str(),
                         identity);
  for (const IncludeStateIdentity &identity : facts.includeStateEvents)
    appendIncludeIdentity(os, llvm::formatv("{0}.include=", bucket).str(),
                          identity);
  for (const IncludeGuardStateIdentity &identity :
       facts.includeGuardStateEvents)
    appendIncludeGuardIdentity(
        os, llvm::formatv("{0}.include_guard=", bucket).str(), identity);
  for (const ConditionalStateIdentity &identity : facts.conditionalStateEvents)
    appendConditionalIdentity(
        os, llvm::formatv("{0}.conditional=", bucket).str(), identity);
  for (const MissingStateFact &fact : facts.missingStateFacts)
    appendMissingFact(os, llvm::formatv("{0}.missing=", bucket).str(), fact);
}

/// Append the four-bucket delta signature (entry / observes / mutates / exit)
/// under the shared prefix `prefix`.
void appendStateDeltaSignature(llvm::raw_ostream &os, llvm::StringRef prefix,
                               const OwnerStateDelta &delta) {
  appendFullStateFacts(os, llvm::formatv("{0}.entry", prefix).str(),
                       delta.entry);
  appendFullStateFacts(os, llvm::formatv("{0}.observes", prefix).str(),
                       delta.observes);
  appendFullStateFacts(os, llvm::formatv("{0}.mutates", prefix).str(),
                       delta.mutates);
  appendFullStateFacts(os, llvm::formatv("{0}.exit", prefix).str(), delta.exit);
}

// --- pure path/fact predicates --------------------------------------------

/// True for the accepted-path kinds that can carry the macro-repair / replay
/// "state-neutral" suffix-state proof.  Only invocation-preserving macro
/// repairs qualify; everything else (paste/stringification/whole-cover/
/// include/TU/terminal) carries its own component-specific proof.
bool pathIsMacroRepairReplayStateNeutralCandidate(AcceptedPathKind path) {
  switch (path) {
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
    return true;
  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    return false;
  }
  return false;
}

/// True iff every fact in `facts` is either a macro requirement, expansion
/// observation, defined-operator observation, or conditional-macro observation
/// — and no missing, line-control, builtin-location, counter, pragma,
/// include, include-guard, or conditional facts are present.
bool factsAreKnownMacroObservationOnly(const OwnerStateFacts &facts) {
  return !facts.HasMissingStateFacts() && !facts.HasMacroDefinitions() &&
         !facts.HasMacroUndefinitions() && !facts.HasLineControlEvents() &&
         !facts.HasBuiltinLocationObservations() && !facts.HasCounterEvents() &&
         !facts.HasPragmaStateEvents() && !facts.HasIncludeStateEvents() &&
         !facts.HasIncludeGuardStateEvents() &&
         !facts.HasConditionalStateEvents();
}

/// Builtin-location observations read line/file/file-name state but do not
/// mutate preprocessing state.  They may coexist with producer-proven macro
/// observations required to explain the macro expansion that produced the
/// builtin, but they must not be used to discharge source-authored #line
/// mutations, counters, include/conditional/pragma effects, or any missing
/// producer fact.  True only when those stricter exclusions hold.
bool factsAreKnownBuiltinLocationObservationOnly(const OwnerStateFacts &facts) {
  return !facts.HasMissingStateFacts() && !facts.HasMacroDefinitions() &&
         !facts.HasMacroUndefinitions() && !facts.HasLineControlEvents() &&
         !facts.HasCounterEvents() && !facts.HasPragmaStateEvents() &&
         !facts.HasIncludeStateEvents() &&
         !facts.HasIncludeGuardStateEvents() &&
         !facts.HasConditionalStateEvents();
}

/// True iff `facts` carries at least one builtin-location observation.
bool factsHaveBuiltinLocationObservation(const OwnerStateFacts &facts) {
  return facts.HasBuiltinLocationObservations();
}

/// True iff `facts` carries include or include-guard events.
bool hasIncludeFacts(const OwnerStateFacts &facts) {
  return facts.HasIncludeStateEvents() || facts.HasIncludeGuardStateEvents();
}

/// True iff `facts` carries any include-guard observation classified as
/// `UnknownGuardEffect` — those are the ones that cannot be matched against
/// a known guard pattern and must remain a fail-closed theorem obligation.
bool hasUnknownIncludeGuardFacts(const OwnerStateFacts &facts) {
  return llvm::any_of(facts.includeGuardStateEvents,
                      [](const IncludeGuardStateIdentity &identity) {
                        return identity.kind ==
                               IncludeGuardObservationKind::UnknownGuardEffect;
                      });
}

/// True iff `facts` carries conditional-state events.
bool hasConditionalFacts(const OwnerStateFacts &facts) {
  return facts.HasConditionalStateEvents();
}

/// True iff `facts` carries any conditional event whose truth is not
/// producer-proven, or that would require reverse-solving an inactive arm.
bool hasUnknownConditionalFacts(const OwnerStateFacts &facts) {
  return llvm::any_of(facts.conditionalStateEvents,
                      [](const ConditionalStateIdentity &identity) {
                        return !identity.conditionTruthProducerProven ||
                               identity.reverseSolvedDirectiveRequired;
                      });
}

/// True iff `facts` describes a known conditional-path state: producer-proven
/// branch events plus the macro observations needed to evaluate the
/// conditions, with no unrelated component facts mixed in.
bool factsAreKnownConditionalPathState(const OwnerStateFacts &facts) {
  return !facts.HasMissingStateFacts() && !facts.HasMacroDefinitions() &&
         !facts.HasMacroUndefinitions() && !facts.HasLineControlEvents() &&
         !facts.HasBuiltinLocationObservations() && !facts.HasCounterEvents() &&
         !facts.HasPragmaStateEvents() && !facts.HasIncludeStateEvents() &&
         !facts.HasIncludeGuardStateEvents() &&
         !hasUnknownConditionalFacts(facts);
}

/// True iff any of the four delta buckets carries a missing-state-fact entry.
bool deltaHasAnyMissingFacts(const OwnerStateDelta &delta) {
  return delta.entry.HasMissingStateFacts() ||
         delta.observes.HasMissingStateFacts() ||
         delta.mutates.HasMissingStateFacts() ||
         delta.exit.HasMissingStateFacts();
}

/// True iff any of the four delta buckets carries include or include-guard
/// events.
bool deltaHasIncludeFacts(const OwnerStateDelta &delta) {
  return hasIncludeFacts(delta.entry) || hasIncludeFacts(delta.observes) ||
         hasIncludeFacts(delta.mutates) || hasIncludeFacts(delta.exit);
}

/// True iff any of the four delta buckets carries an unknown include-guard
/// observation.
bool deltaHasUnknownIncludeGuardFacts(const OwnerStateDelta &delta) {
  return hasUnknownIncludeGuardFacts(delta.entry) ||
         hasUnknownIncludeGuardFacts(delta.observes) ||
         hasUnknownIncludeGuardFacts(delta.mutates) ||
         hasUnknownIncludeGuardFacts(delta.exit);
}

/// True iff any of the four delta buckets carries conditional events.
bool deltaHasConditionalFacts(const OwnerStateDelta &delta) {
  return hasConditionalFacts(delta.entry) ||
         hasConditionalFacts(delta.observes) ||
         hasConditionalFacts(delta.mutates) || hasConditionalFacts(delta.exit);
}

/// True iff any of the four delta buckets carries an unknown conditional
/// event.
bool deltaHasUnknownConditionalFacts(const OwnerStateDelta &delta) {
  return hasUnknownConditionalFacts(delta.entry) ||
         hasUnknownConditionalFacts(delta.observes) ||
         hasUnknownConditionalFacts(delta.mutates) ||
         hasUnknownConditionalFacts(delta.exit);
}

/// True iff `facts` carries counter events or explicitly records that counter
/// facts are missing.  Both shapes establish a counter proof obligation that
/// must be discharged before the suffix-counter dimension is resolver-known.
bool stateFactsHaveCounterProofObligation(const OwnerStateFacts &facts) {
  return facts.HasCounterEvents() ||
         facts.HasMissingFactKind(MissingStateFactKind::MissingCounterFacts);
}

/// True iff any delta bucket carries a counter proof obligation.
bool deltaHasCounterProofObligation(const OwnerStateDelta &delta) {
  return stateFactsHaveCounterProofObligation(delta.entry) ||
         stateFactsHaveCounterProofObligation(delta.observes) ||
         stateFactsHaveCounterProofObligation(delta.mutates) ||
         stateFactsHaveCounterProofObligation(delta.exit);
}

/// True iff any delta bucket explicitly records that counter facts are
/// missing.
bool deltaHasMissingCounterProof(const OwnerStateDelta &delta) {
  return delta.entry.HasMissingFactKind(
             MissingStateFactKind::MissingCounterFacts) ||
         delta.observes.HasMissingFactKind(
             MissingStateFactKind::MissingCounterFacts) ||
         delta.mutates.HasMissingFactKind(
             MissingStateFactKind::MissingCounterFacts) ||
         delta.exit.HasMissingFactKind(
             MissingStateFactKind::MissingCounterFacts);
}

/// True iff `proof` carries at least one suffix-stability witness that names
/// the Counter state component and is not a None / TerminalStateFailure
/// witness.
bool transitionHasStableCounterWitness(const StateTransitionProof &proof) {
  return llvm::any_of(
      proof.suffixWitnesses, [](const SuffixStabilityWitness &suffix) {
        return RefoldOwnerStateProof::SuffixStabilityWitnessNamesComponent(
                   suffix, OwnerStateComponent::Counter) &&
               suffix.kind != SuffixStabilityWitnessKind::None &&
               suffix.kind != SuffixStabilityWitnessKind::TerminalStateFailure;
      });
}

// --- mixed-owner / owner-realization predicates ---------------------------

/// True iff every segment in `tiling` either has no counter obligation or
/// discharges its counter obligation via a stable counter witness on the
/// segment's owner-transition proof.  Missing counter proofs anywhere along
/// the tiling immediately disqualify the tiling.
bool mixedOwnerTilingHasKnownCounterState(
    const MixedOwnerTilingWitness &tiling) {
  if (!tiling.stateSummariesComposed || tiling.segments.empty())
    return false;

  bool sawCounterObligation = false;
  for (const MixedOwnerTilingSegmentWitness &segment : tiling.segments) {
    const StateTransitionProof &proof = segment.ownerTransitionProof;
    if (deltaHasMissingCounterProof(proof.before) ||
        deltaHasMissingCounterProof(proof.after))
      return false;

    const bool segmentHasCounterObligation =
        deltaHasCounterProofObligation(proof.before) ||
        deltaHasCounterProofObligation(proof.after);
    if (!segmentHasCounterObligation)
      continue;

    sawCounterObligation = true;
    if (!transitionHasStableCounterWitness(proof))
      return false;
  }

  return !sawCounterObligation || tiling.compositionEdgesProven;
}

/// True when an include / materialized-expansion realization carries
/// resolver-comparable include-state facts: complete in/out facts, no unknown
/// pragma state, no unknown include-guard observations, and at least one
/// include event somewhere in the delta.
bool ownerRealizationHasKnownIncludeState(
    const OwnerRealizationWitness &owner) {
  const bool includeRealization =
      owner.evidence == OwnerRealizationEvidenceKind::IncludeBEnvelope ||
      owner.evidence ==
          OwnerRealizationEvidenceKind::IncludeMaterializedExpansion;
  if (!includeRealization || !owner.closure.IsComplete())
    return false;

  if (deltaHasAnyMissingFacts(owner.closure.stateIn) ||
      deltaHasAnyMissingFacts(owner.closure.stateOut) ||
      RefoldOwnerStateProof::OwnerStateDeltaHasUnknownPragmaState(
          owner.closure.stateIn) ||
      RefoldOwnerStateProof::OwnerStateDeltaHasUnknownPragmaState(
          owner.closure.stateOut) ||
      deltaHasUnknownIncludeGuardFacts(owner.closure.stateIn) ||
      deltaHasUnknownIncludeGuardFacts(owner.closure.stateOut))
    return false;

  return deltaHasIncludeFacts(owner.closure.stateIn) ||
         deltaHasIncludeFacts(owner.closure.stateOut);
}

/// True when an owner realization carries known active conditional-path
/// state: every conditional event is producer-selected/evaluated, no inactive
/// arm is being reverse-solved, and no unrelated component facts (line,
/// counter, include, pragma) are mixed in.
bool ownerRealizationHasKnownConditionalState(
    const OwnerRealizationWitness &owner) {
  if (!owner.closure.IsComplete())
    return false;

  if (deltaHasAnyMissingFacts(owner.closure.stateIn) ||
      deltaHasAnyMissingFacts(owner.closure.stateOut) ||
      deltaHasUnknownConditionalFacts(owner.closure.stateIn) ||
      deltaHasUnknownConditionalFacts(owner.closure.stateOut) ||
      RefoldOwnerStateProof::OwnerStateDeltaHasUnknownPragmaState(
          owner.closure.stateIn) ||
      RefoldOwnerStateProof::OwnerStateDeltaHasUnknownPragmaState(
          owner.closure.stateOut))
    return false;

  return (deltaHasConditionalFacts(owner.closure.stateIn) ||
          deltaHasConditionalFacts(owner.closure.stateOut)) &&
         factsAreKnownConditionalPathState(owner.closure.stateIn.entry) &&
         factsAreKnownConditionalPathState(owner.closure.stateIn.observes) &&
         factsAreKnownConditionalPathState(owner.closure.stateIn.mutates) &&
         factsAreKnownConditionalPathState(owner.closure.stateIn.exit) &&
         factsAreKnownConditionalPathState(owner.closure.stateOut.entry) &&
         factsAreKnownConditionalPathState(owner.closure.stateOut.observes) &&
         factsAreKnownConditionalPathState(owner.closure.stateOut.mutates) &&
         factsAreKnownConditionalPathState(owner.closure.stateOut.exit);
}

/// True when both stateIn and stateOut deltas observe macros only — no state
/// mutation, no other component facts.
bool ownerRealizationHasKnownMacroObservationOnlyState(
    const OwnerRealizationWitness &owner) {
  const OwnerStateDelta &in = owner.closure.stateIn;
  const OwnerStateDelta &out = owner.closure.stateOut;
  return !RefoldOwnerStateProof::OwnerStateDeltaMutatesAnyState(in) &&
         !RefoldOwnerStateProof::OwnerStateDeltaMutatesAnyState(out) &&
         factsAreKnownMacroObservationOnly(in.entry) &&
         factsAreKnownMacroObservationOnly(in.observes) && in.mutates.Empty() &&
         in.exit.Empty() && factsAreKnownMacroObservationOnly(out.entry) &&
         factsAreKnownMacroObservationOnly(out.observes) &&
         out.mutates.Empty() && out.exit.Empty();
}

/// True when both stateIn and stateOut deltas observe builtin locations only,
/// optionally alongside producer-proven macro observations, with no state
/// mutation and no other component facts.
bool ownerRealizationHasKnownBuiltinLocationObservationState(
    const OwnerRealizationWitness &owner) {
  if (!owner.closure.IsComplete())
    return false;

  const OwnerStateDelta &in = owner.closure.stateIn;
  const OwnerStateDelta &out = owner.closure.stateOut;
  if (RefoldOwnerStateProof::OwnerStateDeltaMutatesAnyState(in) ||
      RefoldOwnerStateProof::OwnerStateDeltaMutatesAnyState(out))
    return false;

  const bool observesBuiltinLocation =
      factsHaveBuiltinLocationObservation(in.entry) ||
      factsHaveBuiltinLocationObservation(in.observes) ||
      factsHaveBuiltinLocationObservation(out.entry) ||
      factsHaveBuiltinLocationObservation(out.observes);
  if (!observesBuiltinLocation)
    return false;

  return factsAreKnownBuiltinLocationObservationOnly(in.entry) &&
         factsAreKnownBuiltinLocationObservationOnly(in.observes) &&
         in.mutates.Empty() && in.exit.Empty() &&
         factsAreKnownBuiltinLocationObservationOnly(out.entry) &&
         factsAreKnownBuiltinLocationObservationOnly(out.observes) &&
         out.mutates.Empty() && out.exit.Empty();
}

// --- signature builders ---------------------------------------------------

/// Build the deterministic signature for an owner-realization witness:
/// owner kind, evidence, source/A/B token ranges, both deltas, and every
/// suffix-stability witness named by component.  Returned as an
/// FNV-1a-stable hex digest via RefoldWitnessTrace::FormatWitnessTraceHash.
std::string ownerStateDeltaSignature(const OwnerRealizationWitness &owner) {
  std::string storage;
  llvm::raw_string_ostream os(storage);
  os << "owner=" << toString(owner.closure.owner.kind)
     << ":evidence=" << toString(owner.evidence);
  if (owner.closure.source.HasPath())
    os << ":source=" << owner.closure.source.path << '['
       << owner.closure.source.begin << ',' << owner.closure.source.end << ')';
  if (owner.closure.aTokens.IsValid())
    os << ":A=[" << owner.closure.aTokens.begin << ','
       << owner.closure.aTokens.end << ')';
  if (owner.closure.bTokens.IsValid())
    os << ":B=[" << owner.closure.bTokens.begin << ','
       << owner.closure.bTokens.end << ')';
  appendStateDeltaSignature(os, "in", owner.closure.stateIn);
  appendStateDeltaSignature(os, "out", owner.closure.stateOut);
  for (const SuffixStabilityWitness &suffix : owner.stateWitnesses)
    os << "state_witness{" << toString(suffix.kind) << ':'
       << toString(
              RefoldOwnerStateProof::ComponentNamedBySuffixStabilityWitness(
                  suffix))
       << "};";
  os.flush();
  return RefoldWitnessTrace::FormatWitnessTraceHash(storage);
}

/// Build the deterministic signature for a mixed-owner tiling witness: the
/// envelope facts, segment summaries (kind, A/B ranges, owner/source/producer
/// signatures), per-segment owner-transition deltas, and suffix witnesses.
std::string mixedOwnerTilingSignature(const MixedOwnerTilingWitness &tiling) {
  std::string storage;
  llvm::raw_string_ostream os(storage);
  os << "tiling=" << tiling.witnessId << ":A=[" << tiling.originalAStart << ','
     << tiling.originalAEnd << ")"
     << ":B=[" << tiling.originalBStart << ',' << tiling.originalBEnd
     << "):tokens=" << tiling.tokenSegmentCount
     << ":state_gaps=" << tiling.stateGapCount
     << ":state_composed=" << (tiling.stateSummariesComposed ? 1 : 0)
     << ":owners_composed=" << (tiling.ownerBoundariesComposed ? 1 : 0)
     << ":target_composed=" << (tiling.targetTokenStreamComposed ? 1 : 0)
     << ":edges_proven=" << (tiling.compositionEdgesProven ? 1 : 0)
     << ":target=" << tiling.globalTargetPPTokenSignature
     << ":composition=" << tiling.globalCompositionSignature;
  for (const MixedOwnerTilingSegmentWitness &segment : tiling.segments) {
    os << ";segment" << segment.segmentIndex
       << "{kind=" << toString(segment.kind) << ":A=[" << segment.aStart << ','
       << segment.aEnd << "):B=[" << segment.bStart << ',' << segment.bEnd
       << "):zero_gap=" << (segment.zeroTokenStateGap ? 1 : 0)
       << ":empty_b=" << (segment.allowEmptyBEnvelope ? 1 : 0)
       << ":closure=" << (segment.ownerClosureComplete ? 1 : 0)
       << ":owner=" << segment.ownerSignature
       << ":source=" << segment.sourceSignature
       << ":producer=" << segment.producerPathSignature
       << ":target=" << segment.targetPPTokenSignature;
    appendStateDeltaSignature(os, "before",
                              segment.ownerTransitionProof.before);
    appendStateDeltaSignature(os, "after", segment.ownerTransitionProof.after);
    for (const SuffixStabilityWitness &suffix :
         segment.ownerTransitionProof.suffixWitnesses) {
      os << "suffix{" << toString(suffix.kind) << ':'
         << toString(
                RefoldOwnerStateProof::ComponentNamedBySuffixStabilityWitness(
                    suffix))
         << "};";
    }
    os << '}';
  }
  os.flush();
  return RefoldWitnessTrace::FormatWitnessTraceHash(storage);
}

/// Build the deterministic signature for the macro-observation-only buckets
/// of both stateIn and stateOut.  Used as the suffix-state signature when an
/// owner realization observes macros only.
std::string
macroObservationOnlyStateSignature(const OwnerRealizationWitness &owner) {
  std::string storage;
  llvm::raw_string_ostream os(storage);
  appendMacroObservationBucket(os, "in.entry", owner.closure.stateIn.entry);
  appendMacroObservationBucket(os, "in.observes",
                               owner.closure.stateIn.observes);
  appendMacroObservationBucket(os, "out.entry", owner.closure.stateOut.entry);
  appendMacroObservationBucket(os, "out.observes",
                               owner.closure.stateOut.observes);
  os.flush();
  return RefoldWitnessTrace::FormatWitnessTraceHash(storage);
}

// --- candidate-state predicates -------------------------------------------

/// True iff the candidate's counter-state witness establishes a stable suffix
/// counter sequence with a known order and no missing expected B values.
bool counterWitnessProvesSuffixCounterStability(
    const AcceptedResultCandidate &candidate) {
  if (!candidate.hasCounterStateWitness)
    return false;
  const CounterStateWitness &counter = candidate.counterStateWitness;
  return counter.suffixStateStable && counter.counterOrderKnown &&
         !counter.hasMissingExpectedBValues;
}

/// True iff the candidate's line-control observer witness is "invocation-
/// neutral": no source-authored or producer-emitted line directives, no
/// include-return / synthetic resyncs, no missing or unknown operands, and a
/// stable physical layout when one is recorded.  Used to gate the macro
/// repair / replay state-neutral key path.
///
/// Builtin-location observations are allowed because they read the
/// already-proven invocation location; they do not mutate suffix preprocessing
/// state.
bool lineControlWitnessIsInvocationNeutral(
    const AcceptedResultCandidate &candidate) {
  if (!candidate.hasLineControlObserverWitness)
    return true;

  const LineControlObserverWitness &line = candidate.lineControlObserverWitness;

  if (line.hasSourceLineControlState || line.lineControlEventCount != 0 ||
      line.sourceAuthoredLineDirectiveCount != 0 ||
      line.producerEmittedLineDirectiveCount != 0 ||
      line.includeReturnResyncCount != 0 || line.syntheticResyncCount != 0 ||
      line.missingOperandLineControlEventCount != 0 ||
      line.unknownOperandLineControlEventCount != 0)
    return false;

  if (line.physicalLayoutKnown && !line.physicalLayoutStable)
    return false;

  return true;
}

/// True iff the candidate qualifies for the macro repair / replay state-
/// neutral suffix-state key.  Gate sequence: path is a macro repair/replay
/// kind, candidate is a MacroPatch, proof is invocation-preserving, structure
/// is preserved, the local discharge is satisfied (or only the selector-only
/// nested-macro failure remains), the target B-token range is valid, the
/// counter witness proves suffix counter stability, and the line-control
/// witness is invocation-neutral.
bool canUseMacroRepairReplayStateNeutrality(
    const AcceptedResultCandidate &candidate, const ProofSummary &summary,
    AcceptedPathKind path, llvm::ArrayRef<PPTok> bToks) {
  if (!pathIsMacroRepairReplayStateNeutralCandidate(path))
    return false;
  if (candidate.kind != AcceptedResultCandidateKind::MacroPatch)
    return false;
  if (summary.theoremClass != TheoremProofClass::InvocationPreservingProof ||
      summary.realizationMode != RealizationMode::PreserveOriginalStructure ||
      !summary.structurePreserving)
    return false;

  // Nested macro selector candidates may intentionally carry the one local
  // discharge failure that final emission later recertifies away: the proof
  // root is producer-proven but not top-level.  That failure is not a
  // suffix-state, observer, diagnostic, or layout instability; it only says
  // this candidate is selector-local.  Allow the neutral state key for that
  // exact selector certificate while continuing to reject every other
  // undischarged proof.
  const bool proofIsLocallyDischarged =
      summary.discharge.status == ProofDischargeStatus::Discharged;
  const bool proofIsSelectorOnlyNestedMacro =
      AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(candidate);
  if (!proofIsLocallyDischarged && !proofIsSelectorOnlyNestedMacro)
    return false;
  if (!candidate.hasTargetBTokenRange ||
      candidate.targetBTokStart > candidate.targetBTokEnd ||
      candidate.targetBTokEnd > bToks.size())
    return false;
  if (!counterWitnessProvesSuffixCounterStability(candidate))
    return false;
  if (!lineControlWitnessIsInvocationNeutral(candidate))
    return false;
  return true;
}

/// Build the macro-repair-replay state-neutral suffix-state signature: path,
/// root macro id, target B-token range and its content hash, counter facts,
/// and a line-control neutrality flag.  Called only after
/// `canUseMacroRepairReplayStateNeutrality` has accepted the candidate.
std::string macroRepairReplayStateNeutralityValue(
    const AcceptedResultCandidate &candidate, const ProofSummary &summary,
    AcceptedPathKind path, const RefoldSourceMapper &sourceMapper) {
  const uint64_t rootId = candidate.hasRootMacroId ? candidate.rootMacroId
                                                   : summary.proofRootMacroId;
  const CounterStateWitness &counter = candidate.counterStateWitness;
  return llvm::formatv(
             "macro_repair_replay_neutral:path={0}:root={1}:"
             "target=[{2},{3}):target_hash={4}:"
             "counter_suffix={5}:counter_order={6}:"
             "counter_events={7}:counter_suffix_observers={8}:"
             "line_control_neutral={9}:diagnostics=preserve-invocation",
             path, rootId, candidate.targetBTokStart, candidate.targetBTokEnd,
             RefoldWitnessTrace::FormatWitnessTraceHash(
                 sourceMapper.SliceBSource(candidate.targetBTokStart,
                                           candidate.targetBTokEnd)),
             counter.suffixStateStable ? 1 : 0,
             counter.counterOrderKnown ? 1 : 0, counter.counterConsumptionCount,
             counter.preservedSuffixObserverCount,
             lineControlWitnessIsInvocationNeutral(candidate) ? 1 : 0)
      .str();
}

} // namespace

::clang::refold::WitnessEquivalenceKey
RefoldWitnessEquivalenceKeyBuilder::Build(
    const AcceptedResultCandidate &candidate) const {
  WitnessEquivalenceKey key;
  const ProofSummary &summary = candidate.proofSummary;
  const AcceptedPathKind path = summary.inventory.currentPath;

  auto knownHash = [&](llvm::StringRef label, llvm::StringRef text) {
    return WitnessEquivalenceDimension::Known(
        llvm::formatv("{0}:{1}", label,
                      RefoldWitnessTrace::FormatWitnessTraceHash(text))
            .str());
  };

  auto knownRangeHash = [&](llvm::StringRef label, uint64_t begin,
                            uint64_t end) {
    return WitnessEquivalenceDimension::Known(
        llvm::formatv("{0}:[{1},{2}):{3}", label, begin, end,
                      RefoldWitnessTrace::FormatWitnessTraceHash(
                          deps_.sourceMapper.SliceBSource(begin, end)))
            .str());
  };

  // keeps the target-preprocessed-token dimension conservative.  It is
  // known only when an accepted proof already carries a B-token envelope or a
  // zero-token B anchor.  Source-spelling previews are intentionally not
  // treated as token-stream proof, because later resolver passes must not merge
  // two witnesses by comparing edited source bytes in place of produced PP
  // tokens.
  if (candidate.hasTargetBTokenRange &&
      candidate.targetBTokStart <= candidate.targetBTokEnd &&
      candidate.targetBTokEnd <= deps_.bToks.size()) {
    const char *label =
        candidate.hasZeroTokenBoundaryWitness &&
                !candidate.hasGeneratedCalleeReplayWitness &&
                !candidate.hasVariadicCommaWitness &&
                !candidate.hasTokenPasteWitness &&
                !candidate.hasStringificationWitness
            ? "zero_token_b_tokens"
            : (candidate.hasGeneratedCalleeReplayWitness
                   ? "generated_callee_b_tokens"
                   : (candidate.hasVariadicCommaWitness
                          ? "variadic_b_tokens"
                          : (candidate.hasTokenPasteWitness
                                 ? "token_paste_b_tokens"
                                 : (candidate.hasStringificationWitness
                                        ? "stringification_b_tokens"
                                        : (candidate.hasMacroActualRepairWitness
                                               ? "macro_actual_b_tokens"
                                               : "candidate_b_tokens")))));
    key.targetPPTokens = knownRangeHash(label, candidate.targetBTokStart,
                                        candidate.targetBTokEnd);
  } else if (candidate.hasZeroTokenBoundaryWitness &&
             candidate.zeroTokenHasBTokenRange &&
             candidate.zeroTokenBTokStart <= candidate.zeroTokenBTokEnd &&
             candidate.zeroTokenBTokEnd <= deps_.bToks.size()) {
    // Zero-token include/boundary witnesses already carry the exact B-token
    // gap/envelope from the modified preprocessed stream.  Promote that
    // producer-recorded envelope into the target-PP equivalence dimension
    // instead of treating the absence of replacement text as unknown output.
    key.targetPPTokens =
        knownRangeHash("zero_token_b_tokens", candidate.zeroTokenBTokStart,
                       candidate.zeroTokenBTokEnd);
  } else if (summary.hasOwnerRealizationWitness &&
             summary.ownerRealizationWitness.closure.IsComplete()) {
    const OwnerTokenRange bTokens =
        summary.ownerRealizationWitness.closure.bTokens;
    key.targetPPTokens = knownRangeHash("b_tokens", bTokens.begin, bTokens.end);
  } else if (summary.hasMixedOwnerTilingWitness) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    if (tiling.originalBStart <= tiling.originalBEnd)
      key.targetPPTokens = knownRangeHash(
          "mixed_owner_b_tokens", tiling.originalBStart, tiling.originalBEnd);
  } else if (summary.hasIncludeAnchorWitness &&
             summary.includeAnchorWitness.hasFirstPP &&
             summary.includeAnchorWitness.hasLastPP &&
             summary.includeAnchorWitness.firstPP <=
                 summary.includeAnchorWitness.lastPP) {
    key.targetPPTokens = knownRangeHash(
        "include_anchor_b_tokens", summary.includeAnchorWitness.firstPP,
        summary.includeAnchorWitness.lastPP + 1);
  } else if (summary.hasTUAnchorWitness && summary.tuAnchorWitness.hasPPGap) {
    key.targetPPTokens = WitnessEquivalenceDimension::Known(
        llvm::formatv("empty_b_gap:{0}", summary.tuAnchorWitness.ppGap).str());
  } else if (path == AcceptedPathKind::TerminalEmitEditedPreprocessedStream) {
    key.targetPPTokens = knownHash("terminal_full_b", deps_.bSource);
  }

  if (candidate.hasGeneratedCalleeReplayWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "generated_callee_replay:root={0}:final_def={1}:depth={2}:"
            "aliases={3}:chain={4}:replay={5}:mapped={6}:"
            "decoded_payload_evidence_only={7}",
            candidate.generatedCalleeRootMacroId,
            candidate.generatedCalleeFinalDirectiveId,
            candidate.generatedCalleeDepth,
            candidate.generatedCalleeObjectAliasHops,
            candidate.generatedCalleeChainDeterministic ? 1 : 0,
            candidate.generatedCalleeReplacementReplayValidated ? 1 : 0,
            candidate.generatedCalleeSolvedActualsMappedToRoot ? 1 : 0,
            candidate.generatedCalleeDecodedStringLiteralEvidenceOnly ? 1 : 0)
            .str());
  } else if (candidate.hasVariadicCommaWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("variadic:root={0}:formal={1}:arity={2}:orig_missing={3}:"
                      "orig_empty={4}:orig_nonempty={5}:result_missing={6}:"
                      "result_empty={7}:result_nonempty={8}:comma_inserted={9}:"
                      "comma_deleted={10}:gnu_elision={11}:vaopt={12}:"
                      "vaopt_orig={13}:vaopt_result={14}:vaopt_comma_ins={15}:"
                      "vaopt_comma_del={16}:producer={17}:pack={18}",
                      candidate.variadicRootMacroId,
                      candidate.variadicFormalIndex,
                      candidate.variadicArityStable ? 1 : 0,
                      candidate.variadicOriginalMissing ? 1 : 0,
                      candidate.variadicOriginalExplicitEmpty ? 1 : 0,
                      candidate.variadicOriginalNonEmpty ? 1 : 0,
                      candidate.variadicResultMissing ? 1 : 0,
                      candidate.variadicResultExplicitEmpty ? 1 : 0,
                      candidate.variadicResultNonEmpty ? 1 : 0,
                      candidate.variadicCommaInserted ? 1 : 0,
                      candidate.variadicCommaDeleted ? 1 : 0,
                      candidate.variadicGnuCommaElision ? 1 : 0,
                      candidate.variadicVaOptPresent ? 1 : 0,
                      candidate.variadicVaOptOriginallyActive ? 1 : 0,
                      candidate.variadicVaOptResultActive ? 1 : 0,
                      candidate.variadicVaOptCommaIntroduced ? 1 : 0,
                      candidate.variadicVaOptCommaDeleted ? 1 : 0,
                      candidate.variadicProducerSignature,
                      candidate.variadicPackStateSignature)
            .str());
  } else if (candidate.hasTokenPasteWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "token_paste:root={0}:spans={1}:tokens={2}:parts={3}:"
            "arg_parts={4}:literal_parts={5}:left={6}:right={7}:"
            "result_validated={8}:diagnostic_safe={9}:producer={10}:result={"
            "11}",
            candidate.tokenPasteRootMacroId, candidate.tokenPasteSpanCount,
            candidate.tokenPasteTokenCount, candidate.tokenPastePartCount,
            candidate.tokenPasteArgPartCount,
            candidate.tokenPasteLiteralPartCount,
            candidate.tokenPasteHasLeftProducer ? 1 : 0,
            candidate.tokenPasteHasRightProducer ? 1 : 0,
            candidate.tokenPasteResultValidated ? 1 : 0,
            candidate.tokenPasteDiagnosticSafe ? 1 : 0,
            candidate.tokenPasteProducerSignature,
            candidate.tokenPasteResultSignature)
            .str());
  } else if (candidate.hasStringificationWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "stringification:root={0}:spans={1}:args={2}:"
            "ws_normalized={3}:escaped_stable={4}:producer={5}:payload={6}",
            candidate.stringificationRootMacroId,
            candidate.stringificationSpanCount,
            candidate.stringificationArgCount,
            candidate.stringificationWhitespaceNormalized ? 1 : 0,
            candidate.stringificationEscapedSpellingStable ? 1 : 0,
            candidate.stringificationProducerSignature,
            candidate.stringificationCanonicalPayloadSignature)
            .str());
  } else if (candidate.hasZeroTokenBoundaryWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("zero_token:owner={0}:{1}:pp_gap={2}:{3}:anchor={4}:{5}:"
                      "b=[{6},{7}):producer={8}:owner_closed={9}:layout={10}:"
                      "observers={11}:counter={12}:empty_actual={13}:"
                      "replacement_gap={14}:"
                      "paired={15}:tu_anchor={16}:include_boundary={17}:"
                      "directive_gap={18}:"
                      "signature={19}",
                      candidate.zeroTokenOwnerKind, candidate.zeroTokenOwnerId,
                      candidate.zeroTokenHasPPGap ? 1 : 0,
                      candidate.zeroTokenPPGap,
                      candidate.zeroTokenHasSourceAnchor ? 1 : 0,
                      candidate.zeroTokenSourceAnchor,
                      candidate.zeroTokenBTokStart, candidate.zeroTokenBTokEnd,
                      candidate.zeroTokenProducerProven ? 1 : 0,
                      candidate.zeroTokenOwnerClosed ? 1 : 0,
                      candidate.zeroTokenLayoutStable ? 1 : 0,
                      candidate.zeroTokenObserversStable ? 1 : 0,
                      candidate.zeroTokenCounterStable ? 1 : 0,
                      candidate.zeroTokenFromEmptyActual ? 1 : 0,
                      candidate.zeroTokenFromReplacementGap ? 1 : 0,
                      candidate.zeroTokenFromPairedInsertion ? 1 : 0,
                      candidate.zeroTokenFromTUAnchor ? 1 : 0,
                      candidate.zeroTokenFromIncludeBoundary ? 1 : 0,
                      candidate.zeroTokenFromDirectiveLayoutGap ? 1 : 0,
                      candidate.zeroTokenBoundarySignature)
            .str());
  } else if (candidate.hasMacroActualRepairWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_actual_replay:root={0}:whole_envelope={1}:"
                      "definition_tape={2}:arity_stable={3}",
                      candidate.rootMacroId,
                      candidate.macroActualWholeEnvelopeReplayValidated ? 1 : 0,
                      candidate.macroActualDefinitionTapeReplayValidated ? 1
                                                                         : 0,
                      candidate.macroActualArityStable ? 1 : 0)
            .str());
  } else if (canUseMacroRepairReplayStateNeutrality(candidate, summary, path,
                                                    deps_.bToks)) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_repair_replay_state_neutral:{0}",
                      macroRepairReplayStateNeutralityValue(
                          candidate, summary, path, deps_.sourceMapper))
            .str());
  } else if (summary.hasSuffixStabilityWitness) {
    const SuffixStabilityWitness &suffix = summary.suffixStabilityWitness;
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "suffix_witness:{0}:component={1}", suffix.kind,
            RefoldOwnerStateProof::ComponentNamedBySuffixStabilityWitness(
                suffix))
            .str());
  } else if (summary.hasOwnerRealizationWitness) {
    const OwnerRealizationWitness &owner = summary.ownerRealizationWitness;
    if (owner.closure.IsStateNeutral()) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv("owner_realization:evidence={0}:state_neutral",
                        owner.evidence)
              .str());
    } else if (ownerRealizationHasKnownMacroObservationOnlyState(owner)) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv(
              "owner_realization:evidence={0}:macro_observation_only:{1}",
              owner.evidence, macroObservationOnlyStateSignature(owner))
              .str());
    } else if (ownerRealizationHasKnownBuiltinLocationObservationState(owner)) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv(
              "owner_realization:evidence={0}:builtin_location_observation:{1}",
              owner.evidence, ownerStateDeltaSignature(owner))
              .str());
    } else if (ownerRealizationHasKnownIncludeState(owner)) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv("owner_realization:evidence={0}:include_state:{1}",
                        owner.evidence, ownerStateDeltaSignature(owner))
              .str());
    } else if (ownerRealizationHasKnownConditionalState(owner)) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv("owner_realization:evidence={0}:conditional_state:{1}",
                        owner.evidence, ownerStateDeltaSignature(owner))
              .str());
    } else if (!owner.stateWitnesses.empty()) {
      std::string stateSummary =
          llvm::formatv(
              "owner_realization:evidence={0}:state_witnesses={1}:state={2}",
              owner.evidence, owner.stateWitnesses.size(),
              ownerStateDeltaSignature(owner))
              .str();
      for (const SuffixStabilityWitness &suffix : owner.stateWitnesses) {
        stateSummary +=
            llvm::formatv(
                ":{0}/{1}", suffix.kind,
                RefoldOwnerStateProof::ComponentNamedBySuffixStabilityWitness(
                    suffix))
                .str();
      }
      key.suffixState = WitnessEquivalenceDimension::Known(stateSummary);
    } else {
      key.suffixState = WitnessEquivalenceDimension::Unknown(
          "owner-realization-state-delta-not-summarized");
    }
  } else if (summary.hasMixedOwnerTilingWitness &&
             summary.mixedOwnerTilingWitness.stateSummariesComposed) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("mixed_owner_composed:id={0}:segments={1}:state_gaps={2}:"
                      "state={3}",
                      tiling.witnessId, tiling.tokenSegmentCount,
                      tiling.stateGapCount, mixedOwnerTilingSignature(tiling))
            .str());
  }

  if (candidate.hasLineControlObserverWitness) {
    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;
    const std::string stateValue =
        llvm::formatv(
            "line_control_state:events={0}:active={1}:inactive={2}:"
            "producer={3}:missing_operands={4}:unknown_operands={5}:"
            "source_directives={6}:producer_directives={7}:"
            "include_return={8}:synthetic_resync={9}:state={10}:layout={11}",
            line.lineControlEventCount, line.activeLineControlEventCount,
            line.inactiveLineControlEventCount,
            line.producerProvenLineControlEventCount,
            line.missingOperandLineControlEventCount,
            line.unknownOperandLineControlEventCount,
            line.sourceAuthoredLineDirectiveCount,
            line.producerEmittedLineDirectiveCount,
            line.includeReturnResyncCount, line.syntheticResyncCount,
            RefoldWitnessTrace::FormatWitnessTraceHash(line.stateSignature),
            RefoldWitnessTrace::FormatWitnessTraceHash(line.layoutSignature))
            .str();

    if (key.suffixState.known) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv("{0}|{1}", key.suffixState.value, stateValue).str());
    } else if (line.hasSourceLineControlState ||
               line.hasSuffixLineControlDischarge) {
      key.suffixState = WitnessEquivalenceDimension::Unknown(
          llvm::formatv("line-control-suffix-state-partial:{0}",
                        RefoldWitnessTrace::FormatWitnessTraceHash(stateValue))
              .str());
    }
  }

  if (candidate.hasGeneratedCalleeReplayWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "generated_callee_replay:root={0}:final_def={1}:target=[{2},{3}):"
            "reexpanded-target-pp",
            candidate.generatedCalleeRootMacroId,
            candidate.generatedCalleeFinalDirectiveId,
            candidate.targetBTokStart, candidate.targetBTokEnd)
            .str());
  } else if (candidate.hasVariadicCommaWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "variadic:root={0}:target=[{1},{2}):formal={3}:"
            "missing={4}:empty={5}:nonempty={6}:literal_comma={7}:"
            "vaopt_result={8}:vaopt_included={9}:reexpanded-target-pp",
            candidate.variadicRootMacroId, candidate.targetBTokStart,
            candidate.targetBTokEnd, candidate.variadicFormalIndex,
            candidate.variadicResultMissing ? 1 : 0,
            candidate.variadicResultExplicitEmpty ? 1 : 0,
            candidate.variadicResultNonEmpty ? 1 : 0,
            candidate.variadicLiteralCommaInActual ? 1 : 0,
            candidate.variadicVaOptResultActive ? 1 : 0,
            candidate.variadicVaOptIncludedCount)
            .str());
  } else if (candidate.hasTokenPasteWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "token_paste:root={0}:target=[{1},{2}):"
            "valid-token-formation={3}:diagnostic-safe={4}:result={5}",
            candidate.tokenPasteRootMacroId, candidate.targetBTokStart,
            candidate.targetBTokEnd,
            candidate.tokenPasteResultValidated ? 1 : 0,
            candidate.tokenPasteDiagnosticSafe ? 1 : 0,
            candidate.tokenPasteResultSignature)
            .str());
  } else if (candidate.hasStringificationWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("stringification:root={0}:target=[{1},{2}):"
                      "escaped-spelling={3}:whitespace={4}:payload={5}",
                      candidate.stringificationRootMacroId,
                      candidate.targetBTokStart, candidate.targetBTokEnd,
                      candidate.stringificationEscapedSpellingStable ? 1 : 0,
                      candidate.stringificationWhitespaceNormalized ? 1 : 0,
                      candidate.stringificationCanonicalPayloadSignature)
            .str());
  } else if (candidate.hasZeroTokenBoundaryWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "zero_token:owner={0}:{1}:target=[{2},{3}):pp_gap={4}:{5}:"
            "source_anchor={6}:{7}:layout={8}:observers={9}:counter={10}:"
            "boundary={11}",
            candidate.zeroTokenOwnerKind, candidate.zeroTokenOwnerId,
            candidate.zeroTokenBTokStart, candidate.zeroTokenBTokEnd,
            candidate.zeroTokenHasPPGap ? 1 : 0, candidate.zeroTokenPPGap,
            candidate.zeroTokenHasSourceAnchor ? 1 : 0,
            candidate.zeroTokenSourceAnchor,
            candidate.zeroTokenLayoutStable ? 1 : 0,
            candidate.zeroTokenObserversStable ? 1 : 0,
            candidate.zeroTokenCounterStable ? 1 : 0,
            candidate.zeroTokenBoundarySignature)
            .str());
  } else if (candidate.hasMacroActualRepairWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_actual_replay:root={0}:target=[{1},{2}):"
                      "reexpanded-target-pp",
                      candidate.rootMacroId, candidate.targetBTokStart,
                      candidate.targetBTokEnd)
            .str());
  } else if (canUseMacroRepairReplayStateNeutrality(candidate, summary, path,
                                                    deps_.bToks)) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_repair_replay_observers_neutral:{0}",
                      macroRepairReplayStateNeutralityValue(
                          candidate, summary, path, deps_.sourceMapper))
            .str());
  } else if (summary.hasTUAnchorWitness) {
    const TUAnchorWitness &w = summary.tuAnchorWitness;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("tu_anchor:evidence={0}:pp_gap={1}:{2}:tu_byte={3}:{4}:"
                      "slot={5}:{6}:macro={7}:left={8}:{9}:right={10}:{11}:"
                      "outside_include={12}:owner_depth_stable={13}",
                      w.evidence, w.hasPPGap, w.ppGap, w.hasTUByte, w.tuByte,
                      w.slotId, w.slotKind, w.macroId, w.hasLeftNeighbor,
                      w.leftNeighborPP, w.hasRightNeighbor, w.rightNeighborPP,
                      w.outsideIncludeCoverage, w.ownerDepthStable)
            .str());
  } else if (summary.hasIncludeAnchorWitness) {
    const IncludeAnchorWitness &w = summary.includeAnchorWitness;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "include_anchor:evidence={0}:anchor={1}:{2}:range={3}:[{4},{5}):"
            "pp_first={6}:{7}:pp_last={8}:{9}:neighbor={10}:{11}:"
            "cond={12}:{13}:child={14}:{15}:decl={16}:[{17},{18})",
            w.evidence, w.hasAnchorByte, w.anchorByte, w.hasByteRange,
            w.startByte, w.endByte, w.hasFirstPP, w.firstPP, w.hasLastPP,
            w.lastPP, w.hasNeighborPP, w.neighborPP, w.hasCondArmId,
            w.condArmId, w.hasChildIncludeId, w.childIncludeId,
            w.hasDeclHeaderRange, w.declHeaderB, w.declHeaderE)
            .str());
  } else if (summary.hasMixedOwnerTilingWitness &&
             summary.mixedOwnerTilingWitness.stateSummariesComposed) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "mixed_owner_observers:id={0}:segments={1}:state_gaps={2}:"
            "state={3}",
            tiling.witnessId, tiling.tokenSegmentCount, tiling.stateGapCount,
            mixedOwnerTilingSignature(tiling))
            .str());
  } else if (summary.hasOwnerRealizationWitness) {
    const OwnerObserverSummary &observers =
        summary.ownerRealizationWitness.closure.observers;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "owner_observers:macro={0}:defined={1}:cond={2}:line={3}:"
            "file={4}:filename={5}:counter={6}:pragma={7}:include_guard={8}:"
            "include={9}",
            observers.observesMacroExpansion, observers.observesDefinedOperator,
            observers.observesConditionalEvaluation,
            observers.observesLineNumber, observers.observesFileState,
            observers.observesFileName, observers.observesCounter,
            observers.observesPragmaState, observers.observesIncludeGuardState,
            observers.observesIncludeState)
            .str());
  } else if (summary.hasSuffixStabilityWitness) {
    const SuffixStabilityWitness &suffix = summary.suffixStabilityWitness;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "suffix_observer_discharge:{0}:component={1}", suffix.kind,
            RefoldOwnerStateProof::ComponentNamedBySuffixStabilityWitness(
                suffix))
            .str());
  }

  if (candidate.hasLineControlObserverWitness) {
    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;
    const std::string observerValue =
        llvm::formatv(
            "line_control_observers:line={0}:file={1}:filename={2}:"
            "builtin={3}:builtin_line={4}:builtin_file={5}:"
            "builtin_filename={6}:suffix_discharge={7}:"
            "obs={8}:layout={9}",
            line.observesLineNumber ? 1 : 0, line.observesFileState ? 1 : 0,
            line.observesFileName ? 1 : 0, line.builtinLocationObservationCount,
            line.builtinLineObservationCount, line.builtinFileObservationCount,
            line.builtinFileNameObservationCount,
            line.hasSuffixLineControlDischarge ? 1 : 0,
            RefoldWitnessTrace::FormatWitnessTraceHash(line.observerSignature),
            RefoldWitnessTrace::FormatWitnessTraceHash(line.layoutSignature))
            .str();

    if (key.preservedObservers.known) {
      key.preservedObservers = WitnessEquivalenceDimension::Known(
          llvm::formatv("{0}|{1}", key.preservedObservers.value, observerValue)
              .str());
    } else {
      key.preservedObservers =
          WitnessEquivalenceDimension::Known(observerValue);
    }
  }

  if (candidate.hasCounterStateWitness) {
    const CounterStateWitness &counter = candidate.counterStateWitness;
    const std::string value =
        llvm::formatv(
            "counter_state:consumes={0}:order_known={1}:observations={2}:"
            "mutations={3}:suffix_observers={4}:suffix_stable={5}:"
            "covers_all={6}:literalized={7}:materialized={8}:"
            "suffix_unobserved={9}:expected_values={10}:missing_values={11}:"
            "order={12}:values={13}:observers={14}",
            counter.counterConsumptionCount, counter.counterOrderKnown ? 1 : 0,
            counter.counterObservationCount, counter.counterMutationCount,
            counter.preservedSuffixObserverCount,
            counter.suffixStateStable ? 1 : 0,
            counter.coversAllAffectedObservers ? 1 : 0,
            counter.literalizationStable ? 1 : 0,
            counter.materializationStable ? 1 : 0,
            counter.suffixUnobserved ? 1 : 0, counter.expectedBValueCount,
            counter.missingExpectedBValueCount,
            RefoldWitnessTrace::FormatWitnessTraceHash(counter.orderSignature),
            RefoldWitnessTrace::FormatWitnessTraceHash(
                counter.suffixValueSignature),
            RefoldWitnessTrace::FormatWitnessTraceHash(
                counter.suffixObserverSignature))
            .str();
    key.counterState = WitnessEquivalenceDimension::Known(value);
  } else if (candidate.hasGeneratedCalleeReplayWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("generated_callee_replay:root={0}:counter-stable",
                      candidate.generatedCalleeRootMacroId)
            .str());
  } else if (candidate.hasVariadicCommaWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("variadic:root={0}:counter-stable",
                      candidate.variadicRootMacroId)
            .str());
  } else if (candidate.hasTokenPasteWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("token_paste:root={0}:counter-stable",
                      candidate.tokenPasteRootMacroId)
            .str());
  } else if (candidate.hasStringificationWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("stringification:root={0}:counter-stable",
                      candidate.stringificationRootMacroId)
            .str());
  } else if (candidate.hasZeroTokenBoundaryWitness) {
    key.counterState =
        candidate.zeroTokenCounterStable
            ? WitnessEquivalenceDimension::Known(
                  llvm::formatv("zero_token:owner={0}:{1}:counter-stable",
                                candidate.zeroTokenOwnerKind,
                                candidate.zeroTokenOwnerId)
                      .str())
            : WitnessEquivalenceDimension::Unknown(
                  "zero-token-counter-stability-not-proven");
  } else if (summary.hasMixedOwnerTilingWitness) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    key.counterState =
        mixedOwnerTilingHasKnownCounterState(tiling)
            ? WitnessEquivalenceDimension::Known(
                  llvm::formatv("mixed_owner_counter_state:id={0}:state={1}",
                                tiling.witnessId,
                                mixedOwnerTilingSignature(tiling))
                      .str())
            : WitnessEquivalenceDimension::Unknown(
                  "mixed-owner-counter-state-not-proven");
  } else if (candidate.hasMacroActualRepairWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_actual_replay:root={0}:counter-stable",
                      candidate.rootMacroId)
            .str());
  } else if (path == AcceptedPathKind::MacroCounterLiteral) {
    key.counterState = summary.hasSuffixStabilityWitness
                           ? WitnessEquivalenceDimension::Known(
                                 "counter_literalized_with_suffix_stability")
                           : WitnessEquivalenceDimension::Unknown(
                                 "counter-literal-without-suffix-witness");
  } else if (summary.hasOwnerRealizationWitness &&
             summary.ownerRealizationWitness.closure.IsComplete() &&
             !summary.ownerRealizationWitness.closure.observers
                  .observesCounter) {
    key.counterState = WitnessEquivalenceDimension::Known(
        "owner-realization-no-preserved-counter-observer");
  }

  if (candidate.hasGeneratedCalleeReplayWitness) {
    WitnessProducerKindSet producers;
    producers.Add(WitnessProducerKind::GeneratedCallee);
    if (candidate.generatedCalleeUsesForwarding)
      producers.Add(WitnessProducerKind::Forward);
    if (candidate.generatedCalleeUsesStringification)
      producers.Add(WitnessProducerKind::Stringify);
    if (candidate.generatedCalleeUsesPaste) {
      producers.Add(WitnessProducerKind::PasteLeft);
      producers.Add(WitnessProducerKind::PasteRight);
      producers.Add(WitnessProducerKind::PasteResult);
    }
    if (candidate.generatedCalleeUsesVariadicForwarding)
      producers.Add(WitnessProducerKind::VariadicForward);
    if (candidate.generatedCalleeUsesObjectAlias)
      producers.Add(WitnessProducerKind::ObjectAlias);
    if (candidate.hasZeroTokenBoundaryWitness)
      producers.Add(WitnessProducerKind::ZeroTokenAnchor);
    key.producerKinds = std::move(producers);
  } else if (candidate.hasVariadicCommaWitness) {
    WitnessProducerKindSet producers;
    producers.Add(WitnessProducerKind::Forward);
    producers.Add(WitnessProducerKind::VariadicForward);
    if (candidate.variadicResultMissing)
      producers.Add(WitnessProducerKind::VariadicMissing);
    if (candidate.variadicResultExplicitEmpty)
      producers.Add(WitnessProducerKind::VariadicEmpty);
    if (candidate.variadicCommaInserted ||
        candidate.variadicVaOptCommaIntroduced)
      producers.Add(WitnessProducerKind::VariadicCommaInsertion);
    if (candidate.variadicCommaDeleted || candidate.variadicGnuCommaElision ||
        candidate.variadicVaOptCommaDeleted)
      producers.Add(WitnessProducerKind::VariadicCommaElision);
    if (candidate.variadicVaOptPresent) {
      if (candidate.variadicVaOptResultActive)
        producers.Add(WitnessProducerKind::VaOptActivation);
      else
        producers.Add(WitnessProducerKind::VaOptErasure);
    }
    if (candidate.hasZeroTokenBoundaryWitness)
      producers.Add(WitnessProducerKind::ZeroTokenAnchor);
    key.producerKinds = std::move(producers);
  } else if (candidate.hasStringificationWitness ||
             candidate.hasTokenPasteWitness) {
    WitnessProducerKindSet producers;
    if (candidate.hasStringificationWitness)
      producers.Add(WitnessProducerKind::Stringify);
    if (candidate.hasTokenPasteWitness) {
      if (candidate.tokenPasteHasLeftProducer)
        producers.Add(WitnessProducerKind::PasteLeft);
      if (candidate.tokenPasteHasRightProducer)
        producers.Add(WitnessProducerKind::PasteRight);
      producers.Add(WitnessProducerKind::PasteResult);
    }
    if (candidate.hasZeroTokenBoundaryWitness)
      producers.Add(WitnessProducerKind::ZeroTokenAnchor);
    key.producerKinds = std::move(producers);
  } else if (candidate.hasZeroTokenBoundaryWitness) {
    WitnessProducerKindSet producers;
    producers.Add(WitnessProducerKind::Forward);
    producers.Add(WitnessProducerKind::ZeroTokenAnchor);
    key.producerKinds = std::move(producers);
  } else {
    key.producerKinds = WitnessProducerKindSet::KnownSingle(
        witnessProducerKindForAcceptedPath(path));
  }

  if (candidate.hasLineControlObserverWitness && key.producerKinds.known) {
    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;
    if (line.lineControlEventCount || line.sourceAuthoredLineDirectiveCount ||
        line.producerEmittedLineDirectiveCount)
      key.producerKinds.Add(WitnessProducerKind::DirectiveMaterialization);
    if (line.builtinLocationObservationCount)
      key.producerKinds.Add(WitnessProducerKind::BuiltinMaterialization);
  }

  if (candidate.hasCounterStateWitness && key.producerKinds.known) {
    const CounterStateWitness &counter = candidate.counterStateWitness;
    if (counter.hasCounterEvents || counter.counterConsumptionCount != 0)
      key.producerKinds.Add(WitnessProducerKind::CounterConsumption);
    if (counter.literalizationStable || counter.materializationStable ||
        counter.hasExpectedBValues)
      key.producerKinds.Add(WitnessProducerKind::BuiltinMaterialization);
  }

  key.boundaryClass = candidate.hasZeroTokenBoundaryWitness
                          ? WitnessBoundaryClass::ZeroTokenBoundary
                          : witnessBoundaryClassForAcceptedCandidate(candidate);

  if (path == AcceptedPathKind::TerminalEmitEditedPreprocessedStream) {
    key.diagnosticClass = WitnessDiagnosticClass::TerminalOutOfDomain;
    key.compositionClass = WitnessCompositionClass::Terminal;
  } else if (summary.realizationMode == RealizationMode::RealizeEditedSurface) {
    key.diagnosticClass = WitnessDiagnosticClass::RealizesEditedSurface;
  } else if (summary.realizationMode ==
             RealizationMode::PreserveOriginalStructure) {
    key.diagnosticClass = WitnessDiagnosticClass::PreservesDiagnostics;
  }

  if (summary.hasMixedOwnerTilingWitness ||
      summary.theoremClass == TheoremProofClass::MixedOwnerTilingProof)
    key.compositionClass = WitnessCompositionClass::MixedOwnerTile;
  else if (summary.hasOwnerRealizationWitness ||
           summary.theoremClass == TheoremProofClass::OwnerRealizationProof)
    key.compositionClass = WitnessCompositionClass::OwnerClosed;
  else if (key.compositionClass == WitnessCompositionClass::Unknown &&
           candidate.kind != AcceptedResultCandidateKind::Unknown)
    key.compositionClass = WitnessCompositionClass::LocalOnly;

  return key;
}

} // namespace refold
} // namespace clang

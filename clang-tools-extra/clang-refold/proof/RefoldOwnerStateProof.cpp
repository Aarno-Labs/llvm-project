//===--- RefoldOwnerStateProof.cpp -----------------------------*- C++ -*-===//
//
// Owner-state proof implementation for clang-refold.
//
// This file contains the owner-state delta, graph, suffix-observer, and
// transition-gateway logic.  The service records theorem-facing audit facts
// through RefoldTheoremAudit directly; RefoldEngine only owns construction and
// orchestration.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldWitnessTrace.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldOwnerStateProof::RefoldOwnerStateProof(
    RefoldOwnerStateProofInputs inputs, const RefoldPathIdentity &paths,
    const RefoldTokenTextAnalysis &tokenText,
    const RefoldMacroTopology &macroTopology,
    const RefoldTheoremAudit &theoremAudit,
    const RefoldTerminalProofSink &terminalSink)
    : model_(inputs.model), aToks_(inputs.aToks), lexLang_(inputs.lexLang),
      paths_(paths), tokenText_(tokenText), macroTopology_(macroTopology),
      theoremAudit_(theoremAudit), terminalSink_(terminalSink) {}

bool rawLexerCommentTokenIsComplete(StringRef spelling) {
  if (spelling.starts_with("//"))
    return true;
  if (spelling.starts_with("/*"))
    return spelling.find("*/", 2) != StringRef::npos;
  return false;
}

namespace {

/// Return true iff `text` contains no preprocessing tokens.
static bool sourceTextIsOnlyLexerTrivia(StringRef text,
                                        const LangOptions &lang) {
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      return true;

    if (!token.is(tok::comment))
      return false;

    const size_t tokenBegin = std::min<size_t>(
        token.getLocation().getRawEncoding() - baseLoc.getRawEncoding(),
        text.size());
    const size_t tokenEnd =
        std::min<size_t>(tokenBegin + token.getLength(), text.size());
    if (!rawLexerCommentTokenIsComplete(text.slice(tokenBegin, tokenEnd)))
      return false;
  }
}

static bool consumePragmaIdentifier(StringRef text, size_t &pos, size_t end,
                                    StringRef expected) {
  StringRef actual;
  if (!stringutils::consumeIdentifier(text, pos, end, actual))
    return false;
  return actual == expected;
}

static bool
consumeDiagnosticOptionStringLiteral(StringRef text, size_t &pos, size_t end,
                                     StringRef *spelling = nullptr) {
  stringutils::skipWsNoLF(text, pos, end);
  if (pos >= end || text[pos] != '"')
    return false;

  const size_t begin = pos;
  ++pos;
  while (pos < end) {
    const char c = text[pos++];
    if (c == '\\') {
      if (pos >= end)
        return false;
      ++pos;
      continue;
    }
    if (c == '"') {
      if (spelling)
        *spelling = text.slice(begin, pos);
      return true;
    }
    if (c == '\n' || c == '\r')
      return false;
  }
  return false;
}

static bool diagnosticPragmaSettingAction(StringRef action) {
  return action == "ignored" || action == "warning" || action == "error" ||
         action == "fatal" || action == "remark";
}

} // namespace

bool sourceTextIsOnlyWhitespaceAndCompleteComments(StringRef text,
                                                   const LangOptions &lang) {
  return sourceTextIsOnlyLexerTrivia(text, lang);
}

/// Strictly parse the diagnostic pragma-state sublanguage admitted by the
/// owner-state proof.
std::optional<ParsedDiagnosticPragmaStateDirective>
parseDiagnosticPragmaStateDirective(StringRef text, const LangOptions &lang) {
  const size_t end = text.size();
  size_t pos = 0;

  if (!stringutils::consumeDirectiveHash(text, pos, end))
    return std::nullopt;
  if (!consumePragmaIdentifier(text, pos, end, "pragma"))
    return std::nullopt;

  stringutils::skipWsNoLF(text, pos, end);
  StringRef namespaceName;
  if (!stringutils::consumeIdentifier(text, pos, end, namespaceName))
    return std::nullopt;
  if (namespaceName != "clang" && namespaceName != "GCC")
    return std::nullopt;

  stringutils::skipWsNoLF(text, pos, end);
  if (!consumePragmaIdentifier(text, pos, end, "diagnostic"))
    return std::nullopt;

  stringutils::skipWsNoLF(text, pos, end);
  StringRef action;
  if (!stringutils::consumeIdentifier(text, pos, end, action))
    return std::nullopt;

  ParsedDiagnosticPragmaStateDirective parsed;
  parsed.namespaceName = namespaceName;
  parsed.actionName = action;
  if (action == "push") {
    parsed.action = DiagnosticPragmaStateAction::Push;
  } else if (action == "pop") {
    parsed.action = DiagnosticPragmaStateAction::Pop;
  } else if (diagnosticPragmaSettingAction(action)) {
    StringRef optionSpelling;
    if (!consumeDiagnosticOptionStringLiteral(text, pos, end, &optionSpelling))
      return std::nullopt;
    parsed.action = DiagnosticPragmaStateAction::Setting;
    parsed.optionSpelling = optionSpelling;
  } else {
    return std::nullopt;
  }

  if (!sourceTextIsOnlyWhitespaceAndCompleteComments(text.drop_front(pos),
                                                     lang))
    return std::nullopt;
  return parsed;
}

/// Return true iff two half-open byte/token intervals overlap.
bool intervalsOverlap(uint64_t beginA, uint64_t endA, uint64_t beginB,
                      uint64_t endB) {
  return beginA < endA && beginB < endB && beginA < endB && beginB < endA;
}

OwnerStateDelta RefoldOwnerStateProof::BuildTheoremStateDelta(
    const OwnerStateFacts &facts, const OwnerStateDelta &directDelta) {
  OwnerStateDelta projected;

  auto projectComponentFacts = [&](const OwnerStateFacts &bucket) {
    // Entry/Observes receive state requirements and observations.
    for (const MacroStateIdentity &identity : bucket.macroRequirements) {
      projected.entry.AddMacroRequirement(identity);
      projected.observes.AddMacroRequirement(identity);
    }
    for (const MacroStateObservation &observation :
         bucket.macroExpansionObservations) {
      projected.entry.AddMacroObservation(observation);
      projected.observes.AddMacroObservation(observation);
    }
    for (const MacroStateObservation &observation :
         bucket.definedOperatorObservations) {
      projected.entry.AddMacroObservation(observation);
      projected.observes.AddMacroObservation(observation);
    }
    for (const MacroStateObservation &observation :
         bucket.conditionalMacroObservations) {
      projected.entry.AddMacroObservation(observation);
      projected.observes.AddMacroObservation(observation);
    }
    for (const BuiltinLocationObservation &observation :
         bucket.builtinLocationObservations) {
      projected.entry.AddBuiltinLocationObservation(observation);
      projected.observes.AddBuiltinLocationObservation(observation);
    }
    for (const CounterEventIdentity &identity : bucket.counterEvents) {
      projected.entry.AddCounterObservation(identity);
      projected.observes.AddCounterObservation(identity);
      projected.mutates.AddCounterMutation(identity);
      projected.exit.AddCounterMutation(identity);
    }
    for (const IncludeStateIdentity &identity : bucket.includeStateEvents) {
      projected.entry.AddIncludeStateEvent(identity);
      projected.observes.AddIncludeStateEvent(identity);
      projected.mutates.AddIncludeStateEvent(identity);
      projected.exit.AddIncludeStateEvent(identity);
    }
    for (const IncludeGuardStateIdentity &identity :
         bucket.includeGuardStateEvents) {
      projected.entry.AddIncludeGuardStateEvent(identity);
      projected.observes.AddIncludeGuardStateEvent(identity);
      projected.mutates.AddIncludeGuardStateEvent(identity);
      projected.exit.AddIncludeGuardStateEvent(identity);
    }
    for (const PragmaStateIdentity &identity : bucket.pragmaStateEvents) {
      projected.entry.AddPragmaStateEvent(identity);
      projected.observes.AddPragmaStateEvent(identity);
      projected.mutates.AddPragmaStateEvent(identity);
      projected.exit.AddPragmaStateEvent(identity);
    }
    for (const ConditionalStateIdentity &identity :
         bucket.conditionalStateEvents) {
      projected.entry.AddConditionalStateEvent(identity);
      projected.observes.AddConditionalStateEvent(identity);
      projected.mutates.AddConditionalStateEvent(identity);
      projected.exit.AddConditionalStateEvent(identity);
    }

    // Mutates/Exit receive state transitions.
    for (const MacroStateIdentity &identity : bucket.macroDefinitions) {
      projected.mutates.AddMacroDefinition(identity);
      projected.exit.AddMacroDefinition(identity);
    }
    for (const MacroStateIdentity &identity : bucket.macroUndefinitions) {
      projected.mutates.AddMacroUndefinition(identity);
      projected.exit.AddMacroUndefinition(identity);
    }
    for (const LineControlStateIdentity &identity : bucket.lineControlEvents) {
      projected.mutates.AddLineControlEvent(identity);
      projected.exit.AddLineControlEvent(identity);
    }

    // Missing facts are theorem obligations, so they remain visible in every
    // bucket.  MissingOwnerOrderingFacts remains the precise unmodeled-ordering
    // marker; no flat poison bit is synthesized.
    for (const MissingStateFact &fact : bucket.missingStateFacts) {
      projected.entry.AddMissingStateFact(fact.kind, fact.detail);
      projected.observes.AddMissingStateFact(fact.kind, fact.detail);
      projected.mutates.AddMissingStateFact(fact.kind, fact.detail);
      projected.exit.AddMissingStateFact(fact.kind, fact.detail);
    }
  };

  // Project precise facts attached to the builder surface, then merge precise
  // facts that later proof passes may have attached directly to delta buckets.
  projectComponentFacts(facts);
  OwnerStateDelta preciseDelta;
  preciseDelta.MergeTheoremFactsFrom(directDelta);
  projected.MergeFrom(preciseDelta);
  return projected;
}

bool RefoldOwnerStateProof::OwnerStateDeltaHasUnmodeledState(
    const OwnerStateDelta &delta) {
  return delta.entry.HasMissingFactKind(
             MissingStateFactKind::MissingOwnerOrderingFacts) ||
         delta.observes.HasMissingFactKind(
             MissingStateFactKind::MissingOwnerOrderingFacts) ||
         delta.mutates.HasMissingFactKind(
             MissingStateFactKind::MissingOwnerOrderingFacts) ||
         delta.exit.HasMissingFactKind(
             MissingStateFactKind::MissingOwnerOrderingFacts);
}

bool RefoldOwnerStateProof::OwnerStateDeltaHasUnknownPragmaState(
    const OwnerStateDelta &delta) {
  return delta.entry.HasTheoremUnknownPragmaState() ||
         delta.observes.HasTheoremUnknownPragmaState() ||
         delta.mutates.HasTheoremUnknownPragmaState() ||
         delta.exit.HasTheoremUnknownPragmaState();
}

bool RefoldOwnerStateProof::OwnerStateDeltaMutatesAnyState(
    const OwnerStateDelta &delta) {
  return delta.mutates.MutatesAnyState() || delta.exit.MutatesAnyState();
}

OwnerObserverSummary RefoldOwnerStateProof::OwnerStateDeltaToObserverSummary(
    const OwnerStateDelta &delta) {
  const StateObservations &observations = delta.observes;
  OwnerObserverSummary observers;
  observers.observesMacroExpansion =
      observations.HasMacroRequirements() ||
      observations.HasMacroExpansionObservations();
  observers.observesDefinedOperator =
      observations.HasDefinedOperatorObservations();
  observers.observesConditionalEvaluation =
      observations.HasConditionalMacroObservations() ||
      observations.HasConditionalStateEvents();
  observers.observesLineNumber = llvm::any_of(
      observations.builtinLocationObservations,
      [](const BuiltinLocationObservation &obs) {
        return obs.kind == BuiltinLocationObservationKind::LineState;
      });
  observers.observesFileState = llvm::any_of(
      observations.builtinLocationObservations,
      [](const BuiltinLocationObservation &obs) {
        return obs.kind == BuiltinLocationObservationKind::FileState;
      });
  observers.observesFileName = llvm::any_of(
      observations.builtinLocationObservations,
      [](const BuiltinLocationObservation &obs) {
        return obs.kind == BuiltinLocationObservationKind::FileNameState;
      });
  observers.observesCounter = observations.HasCounterEvents();
  observers.observesPragmaState = observations.HasPragmaStateEvents() ||
                                  observations.HasTheoremUnknownPragmaState();
  observers.observesIncludeGuardState =
      observations.HasIncludeGuardStateEvents();
  observers.observesIncludeState = observations.HasIncludeStateEvents();
  for (const MissingStateFact &fact : observations.missingStateFacts) {
    switch (fact.kind) {
    case MissingStateFactKind::MissingMacroFacts:
      observers.observesMacroExpansion = true;
      observers.observesDefinedOperator = true;
      observers.observesConditionalEvaluation = true;
      break;
    case MissingStateFactKind::MissingLineControlFacts:
      observers.observesLineNumber = true;
      observers.observesFileState = true;
      observers.observesFileName = true;
      break;
    case MissingStateFactKind::MissingCounterFacts:
      observers.observesCounter = true;
      break;
    case MissingStateFactKind::MissingPragmaFacts:
      observers.observesPragmaState = true;
      break;
    case MissingStateFactKind::MissingIncludeGuardFacts:
      observers.observesIncludeGuardState = true;
      break;
    case MissingStateFactKind::MissingConditionalFacts:
      observers.observesConditionalEvaluation = true;
      break;
    case MissingStateFactKind::MissingOwnerOrderingFacts:
      // There is no precise component when the owner/event ordering fact is
      // absent.  The missing fact remains in the delta so later graph/gateway
      // checks can report the ordering obligation explicitly.
      break;
    }
  }
  return observers;
}

DirectStateCheckKind RefoldOwnerStateProof::DirectStateCheckKindForComponent(
    OwnerStateComponent component) {
  switch (component) {
  case OwnerStateComponent::MacroState:
  case OwnerStateComponent::DefinedOperator:
    return DirectStateCheckKind::MacroExpansionState;
  case OwnerStateComponent::ConditionalState:
    return DirectStateCheckKind::ConditionalDirectiveState;
  case OwnerStateComponent::LineNumber:
    return DirectStateCheckKind::BuiltinLineObserver;
  case OwnerStateComponent::FileState:
    return DirectStateCheckKind::BuiltinFileObserver;
  case OwnerStateComponent::FileName:
    return DirectStateCheckKind::BuiltinFileNameObserver;
  case OwnerStateComponent::Counter:
    return DirectStateCheckKind::CounterEvent;
  case OwnerStateComponent::PragmaState:
    return DirectStateCheckKind::PragmaDirective;
  case OwnerStateComponent::IncludeGuardState:
    return DirectStateCheckKind::IncludeGuardState;
  case OwnerStateComponent::IncludeState:
    return DirectStateCheckKind::IncludeDirectiveState;
  case OwnerStateComponent::UnmodeledState:
    return DirectStateCheckKind::UnmodeledStateFact;
  case OwnerStateComponent::Unknown:
    return DirectStateCheckKind::Unknown;
  }
  llvm_unreachable("Invalid owner state component");
}

DirectStateCheckKind RefoldOwnerStateProof::DirectStateCheckKindForGraphNode(
    OwnerStateGraphNodeKind kind) {
  switch (kind) {
  case OwnerStateGraphNodeKind::MacroDefineEvent:
    return DirectStateCheckKind::MacroDefinitionDirective;
  case OwnerStateGraphNodeKind::MacroUndefEvent:
    return DirectStateCheckKind::MacroUndefDirective;
  case OwnerStateGraphNodeKind::LineControlEvent:
    return DirectStateCheckKind::LineControlDirective;
  case OwnerStateGraphNodeKind::PragmaEvent:
    return DirectStateCheckKind::PragmaDirective;
  case OwnerStateGraphNodeKind::ConditionalEvent:
    return DirectStateCheckKind::ConditionalDirectiveState;
  case OwnerStateGraphNodeKind::CounterEvent:
    return DirectStateCheckKind::CounterEvent;
  case OwnerStateGraphNodeKind::IncludeEntry:
  case OwnerStateGraphNodeKind::IncludeExit:
    return DirectStateCheckKind::IncludeDirectiveState;
  case OwnerStateGraphNodeKind::MacroInvocation:
  case OwnerStateGraphNodeKind::NestedMacroExpansion:
    return DirectStateCheckKind::MacroExpansionState;
  case OwnerStateGraphNodeKind::OrdinaryTokenOwner:
  case OwnerStateGraphNodeKind::Unknown:
    return DirectStateCheckKind::Unknown;
  }
  llvm_unreachable("Invalid owner state graph node kind");
}

OwnerStateComponent RefoldOwnerStateProof::DirectStateComponentForGraphNode(
    OwnerStateGraphNodeKind kind) {
  switch (kind) {
  case OwnerStateGraphNodeKind::MacroDefineEvent:
  case OwnerStateGraphNodeKind::MacroUndefEvent:
  case OwnerStateGraphNodeKind::MacroInvocation:
  case OwnerStateGraphNodeKind::NestedMacroExpansion:
    return OwnerStateComponent::MacroState;
  case OwnerStateGraphNodeKind::LineControlEvent:
    return OwnerStateComponent::LineNumber;
  case OwnerStateGraphNodeKind::PragmaEvent:
    return OwnerStateComponent::PragmaState;
  case OwnerStateGraphNodeKind::ConditionalEvent:
    return OwnerStateComponent::ConditionalState;
  case OwnerStateGraphNodeKind::CounterEvent:
    return OwnerStateComponent::Counter;
  case OwnerStateGraphNodeKind::IncludeEntry:
  case OwnerStateGraphNodeKind::IncludeExit:
    return OwnerStateComponent::IncludeState;
  case OwnerStateGraphNodeKind::OrdinaryTokenOwner:
  case OwnerStateGraphNodeKind::Unknown:
    return OwnerStateComponent::Unknown;
  }
  llvm_unreachable("Invalid owner state graph node kind");
}

void RefoldOwnerStateProof::AuditDirectStateCheckClosure(
    DirectStateCheckKind checkKind, OwnerStateComponent component,
    DirectStateCheckClosureKind closure, StateMutationKind mutation,
    StringRef stage, StringRef detail) const {
  theoremAudit_.RecordDirectStateCheckClosure(checkKind, component, closure,
                                              mutation, stage, detail);
}
bool RefoldOwnerStateProof::OwnerMatchesSourceSite(
    const Owner &owner, StringRef file, std::optional<uint64_t> ownerIncludeId,
    uint64_t begin, uint64_t end) const {
  if (!owner.IsKnown() || owner.IsMacroInvocation())
    return false;

  if (owner.IsConditionalArm()) {
    if (!owner.condArmId)
      return false;
    return macroTopology_.SourceRangeInsideConditionalArm(
        *owner.condArmId, file, ownerIncludeId, begin, end);
  }

  if (owner.IsConditionalGroup()) {
    if (!owner.condGroupId)
      return false;
    const RefoldModel::CondGroup *group =
        model_.GetCondGroupById(*owner.condGroupId);
    return group && paths_.PathsEqual(group->file, file) &&
           group->parentIncludeId == ownerIncludeId && group->groupB <= begin &&
           begin <= end && end <= group->groupE;
  }

  const bool includeOwnerMatches = owner.IsTU()
                                       ? !ownerIncludeId
                                       : (owner.includeId && ownerIncludeId &&
                                          *owner.includeId == *ownerIncludeId);
  if (!includeOwnerMatches)
    return false;

  if (owner.condArmId) {
    return macroTopology_.SourceRangeInsideConditionalArm(
        *owner.condArmId, file, ownerIncludeId, begin, end);
  }

  return true;
}

uint64_t RefoldOwnerStateProof::OwnerIncludeBucketKey(
    std::optional<uint64_t> includeId) {
  // DenseMap has no optional key here.  Reserve bucket 0 for TU/virtual-root
  // facts and shift concrete include ids by one.  Producer ids are uint64_t, so
  // the saturated max value is intentionally folded into the root bucket rather
  // than risking wraparound; such an id would already be outside the normal
  // producer-id domain.
  if (!includeId || *includeId == std::numeric_limits<uint64_t>::max())
    return 0;
  return *includeId + 1;
}

std::optional<uint64_t>
RefoldOwnerStateProof::OwnerSourceBucketKey(const Owner &owner) {
  if (owner.IsTU())
    return OwnerIncludeBucketKey(std::nullopt);
  if (owner.IsInclude() && owner.includeId)
    return OwnerIncludeBucketKey(owner.includeId);
  return std::nullopt;
}

std::string RefoldOwnerStateProof::OwnerStateDeltaCacheKey(const Owner &owner) {
  const uint64_t none = std::numeric_limits<uint64_t>::max();
  return llvm::formatv(
             "{0}:{1}:{2}:{3}:{4}:{5}:{6}:{7}",
             static_cast<unsigned>(owner.kind), owner.includeId.value_or(none),
             owner.macroInvocationId.value_or(none),
             owner.macroDirectiveId.value_or(none),
             owner.lineControlId.value_or(none), owner.pragmaId.value_or(none),
             owner.condGroupId.value_or(none), owner.condArmId.value_or(none))
      .str();
}

bool RefoldOwnerStateProof::IsVirtualInitialMacroDirectiveSource(
    StringRef sitePath) {
  // Clang serializes its predefined macro environment as #define directives in
  // the pseudo-file "<built-in>".  Those directives seed the initial macro
  // state before any source byte in the translation unit exists; they are not
  // source-editable directive islands and cannot be ordered as suffix observers
  // after a TU/header edit boundary.  They remain available through the exact
  // macro-directive index for owner-state accounting, but the graph builder
  // must not manufacture ordinary source-order nodes for them.
  return sitePath == "<built-in>";
}

OwnerStateFactIndex RefoldOwnerStateProof::BuildOwnerStateFactIndex() const {
  OwnerStateFactIndex index;

  for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
    index.includesByParentIncludeKey[OwnerIncludeBucketKey(include.parent)]
        .push_back(&include);
  }

  for (const RefoldModel::MacroDirective &directive :
       model_.GetMacroDirectives()) {
    index.macroDirectiveById[directive.id] = &directive;
    index
        .macroDirectivesByOwnerIncludeKey[OwnerIncludeBucketKey(
            directive.ownerIncludeId)]
        .push_back(&directive);
  }

  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    index.lineControlById[event.id] = &event;
    index
        .lineControlsByOwnerIncludeKey[OwnerIncludeBucketKey(
            event.ownerIncludeId)]
        .push_back(&event);
  }

  for (const RefoldModel::PragmaDirective &pragma : model_.GetPragmas()) {
    index.pragmaById[pragma.id] = &pragma;

    std::optional<uint64_t> ownerIncludeId = pragma.ownerIncludeId;
    if (!ownerIncludeId) {
      // Older producer maps did not serialize owner_include_id for pragmas.
      // Recover the same single-segment owner used by BuildOwnerStateDelta(),
      // but do it once while constructing the immutable fact index.
      for (const RefoldModel::Segment &segment :
           model_.GetSegmentsForFile(pragma.sitePath)) {
        if (segment.b <= pragma.siteB && pragma.siteE <= segment.e) {
          ownerIncludeId = segment.ownerIncludeId;
          break;
        }
      }
    }

    index.pragmasByOwnerIncludeKey[OwnerIncludeBucketKey(ownerIncludeId)]
        .push_back(&pragma);
  }

  for (const RefoldModel::CondGroup &group : model_.GetConds()) {
    index
        .condGroupsByParentIncludeKey[OwnerIncludeBucketKey(
            group.parentIncludeId)]
        .push_back(&group);
  }

  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    index.macroInvocationById[macro.id] = &macro;
    index
        .macroInvocationsByOwnerIncludeKey[OwnerIncludeBucketKey(
            macro.ownerIncludeId)]
        .push_back(&macro);
  }

  return index;
}

const OwnerStateFactIndex &
RefoldOwnerStateProof::GetOwnerStateFactIndex() const {
  if (!ownerStateFactIndexCache_)
    ownerStateFactIndexCache_ = BuildOwnerStateFactIndex();
  return *ownerStateFactIndexCache_;
}

OwnerStateDelta
RefoldOwnerStateProof::GetOwnerStateDelta(const Owner &owner) const {
  if (theoremAudit_.IsNoLegacyAuditEnabled())
    return BuildOwnerStateDelta(owner);

  const std::string key = OwnerStateDeltaCacheKey(owner);
  auto it = ownerStateDeltaCache_.find(key);
  if (it != ownerStateDeltaCache_.end())
    return it->second;

  OwnerStateDelta delta = BuildOwnerStateDelta(owner);
  ownerStateDeltaCache_[key] = delta;
  return ownerStateDeltaCache_.find(key)->second;
}

OwnerClosure
RefoldOwnerStateProof::AttachCanonicalStateSummary(OwnerClosure closure) const {
  OwnerStateDelta summary = GetOwnerStateDelta(closure.owner);
  closure.stateIn = summary;
  closure.stateOut = summary;
  closure.observers = OwnerStateDeltaToObserverSummary(summary);
  return closure;
}

OwnerStateDelta
RefoldOwnerStateProof::BuildOwnerStateDelta(const Owner &owner) const {
  OwnerStateFacts facts;
  const bool auditDirectState = theoremAudit_.IsNoLegacyAuditEnabled();

  auto auditDeltaFact = [&](DirectStateCheckKind checkKind,
                            OwnerStateComponent component, StringRef detail) {
    if (!auditDirectState)
      return;
    AuditDirectStateCheckClosure(
        checkKind, component, DirectStateCheckClosureKind::OwnerStateDeltaFact,
        StateMutationKind::Unknown, "owner-state-delta", detail);
  };

  auto markMissing = [&](MissingStateFactKind kind, StringRef detail) {
    facts.AddMissingStateFact(kind, detail);
    auditDeltaFact(DirectStateCheckKind::UnmodeledStateFact,
                   StateComponentForMissingStateFact(kind), detail);
  };

  auto markMissingAndUnmodeled = [&](MissingStateFactKind kind,
                                     StringRef detail) {
    markMissing(kind, detail);
  };

  auto finalize = [&]() {
    // There is no OwnerStateSummary compatibility bridge.  Build the canonical
    // theorem delta directly from precise producer facts and explicit
    // MissingStateFact markers, so missing facts cannot disappear as an empty
    // flat summary.
    OwnerStateDelta theoremDelta =
        BuildTheoremStateDelta(facts, OwnerStateDelta());
    if (facts.HasMissingStateFacts() && theoremDelta.Empty()) {
      facts.AddMissingStateFact(
          MissingStateFactKind::MissingOwnerOrderingFacts,
          "missing producer facts failed to project into theorem delta");
      theoremDelta = BuildTheoremStateDelta(facts, OwnerStateDelta());
    }
    return theoremDelta;
  };
  if (!owner.IsKnown()) {
    markMissingAndUnmodeled(MissingStateFactKind::MissingOwnerOrderingFacts,
                            "unknown owner identity");
    return finalize();
  }

  auto recordBuiltinLocationObservation =
      [&](BuiltinLocationObservationKind kind,
          std::optional<uint64_t> ownerIncludeId = std::nullopt,
          std::optional<uint64_t> sourceBegin = std::nullopt,
          std::optional<uint64_t> sourceEnd = std::nullopt,
          std::optional<uint64_t> aTokenBegin = std::nullopt,
          std::optional<uint64_t> aTokenEnd = std::nullopt) {
        switch (kind) {
        case BuiltinLocationObservationKind::LineState:
          auditDeltaFact(DirectStateCheckKind::BuiltinLineObserver,
                         OwnerStateComponent::LineNumber,
                         "producer/textual __LINE__ observation");
          break;
        case BuiltinLocationObservationKind::FileState:
          auditDeltaFact(DirectStateCheckKind::BuiltinFileObserver,
                         OwnerStateComponent::FileState,
                         "producer/textual __FILE__ observation");
          break;
        case BuiltinLocationObservationKind::FileNameState:
          auditDeltaFact(DirectStateCheckKind::BuiltinFileNameObserver,
                         OwnerStateComponent::FileName,
                         "producer/textual __FILE_NAME__ observation");
          break;
        }
        BuiltinLocationObservation observation;
        observation.kind = kind;
        observation.ownerIncludeId = ownerIncludeId;
        observation.sourceBegin = sourceBegin;
        observation.sourceEnd = sourceEnd;
        observation.aTokenBegin = aTokenBegin;
        observation.aTokenEnd = aTokenEnd;
        facts.AddBuiltinLocationObservation(observation);
      };

  auto scanTextForBuiltins = [&](StringRef text) {
    if (text.empty())
      return;
    if (tokenText_.RawIdentifierAppearsInText("__LINE__", text))
      recordBuiltinLocationObservation(
          BuiltinLocationObservationKind::LineState);
    if (tokenText_.RawIdentifierAppearsInText("__FILE__", text))
      recordBuiltinLocationObservation(
          BuiltinLocationObservationKind::FileState);
    if (tokenText_.RawIdentifierAppearsInText("__FILE_NAME__", text))
      recordBuiltinLocationObservation(
          BuiltinLocationObservationKind::FileNameState);
    if (tokenText_.RawIdentifierAppearsInText("__COUNTER__", text)) {
      // Textual `__COUNTER__` in source/invocation spelling is a conservative
      // fallback for older producer maps.  Producer-backed counter events are
      // recorded separately from MacroInvocation records below.  Record the
      // missing event fact explicitly so the proof model can fail closed if a
      // proof needs per-event counter identity.
      markMissing(MissingStateFactKind::MissingCounterFacts,
                  "textual __COUNTER__ without producer counter event");
    }
  };

  auto scanPPSpanForBuiltins = [&](const RefoldModel::PPSpan &span) {
    if (!span.IsValid())
      return;
    const uint64_t end = std::min<uint64_t>(span.end, aToks_.size());
    for (uint64_t i = span.begin; i < end; ++i) {
      StringRef spelling = aToks_[static_cast<size_t>(i)].spelling;
      if (tokenText_.RawIdentifierAppearsInText("__LINE__", spelling))
        recordBuiltinLocationObservation(
            BuiltinLocationObservationKind::LineState, std::nullopt,
            std::nullopt, std::nullopt, i, i + 1);
      if (tokenText_.RawIdentifierAppearsInText("__FILE__", spelling))
        recordBuiltinLocationObservation(
            BuiltinLocationObservationKind::FileState, std::nullopt,
            std::nullopt, std::nullopt, i, i + 1);
      if (tokenText_.RawIdentifierAppearsInText("__FILE_NAME__", spelling))
        recordBuiltinLocationObservation(
            BuiltinLocationObservationKind::FileNameState, std::nullopt,
            std::nullopt, std::nullopt, i, i + 1);
      if (tokenText_.RawIdentifierAppearsInText("__COUNTER__", spelling)) {
        markMissing(MissingStateFactKind::MissingCounterFacts,
                    "A-token __COUNTER__ without producer counter event");
      }
    }
    if (span.end > aToks_.size())
      markMissingAndUnmodeled(MissingStateFactKind::MissingOwnerOrderingFacts,
                              "producer PP span extends past A-token stream");
  };

  auto macroReplacementTokenFingerprint =
      [&](ArrayRef<RefoldModel::MacroReplacementToken> tokens) -> std::string {
    std::string fingerprint;
    llvm::raw_string_ostream os(fingerprint);
    for (const RefoldModel::MacroReplacementToken &token : tokens) {
      // This is a deterministic producer-fact fingerprint, not a cryptographic
      // hash.  It is intentionally textual so theorem-audit logs can expose the
      // exact definition-shape evidence without reparsing the directive body.
      os << static_cast<unsigned>(token.kind) << ':' << token.spelling.size()
         << ':' << token.spelling << ':';
      if (token.paramIndex)
        os << 'P' << *token.paramIndex;
      else
        os << '-';
      os << ';';
    }
    return os.str();
  };

  auto anyVariadicParam = [](ArrayRef<RefoldModel::MacroDefParam> params) {
    return llvm::any_of(params, [](const RefoldModel::MacroDefParam &param) {
      return param.variadic;
    });
  };

  auto identityFromDirective =
      [&](const RefoldModel::MacroDirective &directive) -> MacroStateIdentity {
    MacroStateIdentity identity;
    identity.macroName = directive.name.str();
    if (directive.subkind == "#define")
      identity.definitionDirectiveId = directive.id;
    else if (directive.subkind == "#undef")
      identity.undefDirectiveId = directive.id;
    identity.functionLike = directive.functionLike;
    identity.arity = static_cast<uint32_t>(directive.defParams.size());
    identity.variadic = anyVariadicParam(directive.defParams);
    identity.replacementTokenHash =
        macroReplacementTokenFingerprint(directive.replacementTokens);
    return identity;
  };

  auto identityFromInvocation =
      [&](const RefoldModel::MacroInvocation &macro) -> MacroStateIdentity {
    MacroStateIdentity identity;
    identity.macroName = macro.name.str();
    identity.definitionDirectiveId = macro.definitionDirectiveId;
    identity.functionLike = macro.subkind == "func";
    identity.arity = static_cast<uint32_t>(macro.defParams.size());
    identity.variadic = anyVariadicParam(macro.defParams);
    return identity;
  };

  auto identityFromMacroName = [](StringRef name) -> MacroStateIdentity {
    MacroStateIdentity identity;
    identity.macroName = name.str();
    return identity;
  };

  auto recordMacroObservation = [&](MacroObservationKind kind,
                                    const MacroStateIdentity &identity) {
    auditDeltaFact(kind == MacroObservationKind::DefinedOperator
                       ? DirectStateCheckKind::MacroDefinitionDirective
                       : DirectStateCheckKind::MacroExpansionState,
                   kind == MacroObservationKind::DefinedOperator
                       ? OwnerStateComponent::DefinedOperator
                       : OwnerStateComponent::MacroState,
                   "macro-state observation");
    MacroStateObservation observation;
    observation.kind = kind;
    observation.identity = identity;
    facts.AddMacroObservation(observation);
  };

  auto lineControlIdentityFromEvent =
      [&](const RefoldModel::LineControlEvent &event)
      -> LineControlStateIdentity {
    LineControlStateIdentity identity;
    identity.eventId = event.id;
    identity.physicalFile = event.physicalFile.str();
    identity.siteBegin = event.siteB;
    identity.siteEnd = event.siteE;
    identity.active = event.active;
    identity.producerProven = event.producerProven;
    identity.logicalLineAfter = event.logicalLineAfter;
    identity.logicalFileAfter = event.logicalFileAfter.str();
    identity.ownerIncludeId = event.ownerIncludeId;
    if (event.active && event.producerProven)
      identity.operandProvenance =
          LineDirectiveOperandProvenance::ProducerEvaluatedOperands;
    else if (!event.active)
      identity.operandProvenance =
          LineDirectiveOperandProvenance::InactiveDirective;
    else if (!event.producerProven)
      identity.operandProvenance =
          LineDirectiveOperandProvenance::MissingProducerOperands;
    else
      identity.operandProvenance = LineDirectiveOperandProvenance::Unknown;
    return identity;
  };

  auto includeStateIdentityFromInclude =
      [&](const RefoldModel::IncludeItem &include) -> IncludeStateIdentity {
    IncludeStateIdentity identity;
    identity.includeId = include.id;
    identity.directiveKind = include.subkind.str();
    identity.sitePath = include.sitePath.str();
    identity.siteBegin = include.siteB;
    identity.siteEnd = include.siteE;
    identity.target = include.target.str();
    if (include.resolvedPath)
      identity.resolvedPath = include.resolvedPath->str();
    identity.angled = include.angled;
    identity.parentIncludeId = include.parent;
    identity.hasTokenMaterialization = include.cover.IsValid();
    return identity;
  };

  auto includeGuardIdentityFromInclude =
      [&](const RefoldModel::IncludeItem &include)
      -> IncludeGuardStateIdentity {
    IncludeGuardStateIdentity identity;
    identity.includeId = include.id;
    identity.headerPath = include.resolvedPath ? include.resolvedPath->str()
                                               : include.target.str();
    identity.parentIncludeId = include.parent;
    // The current producer map records include identity and conditional/macro
    // events, but not a first-class include-guard oracle.  Keep the guard macro
    // absent and producerProvenGuard=false rather than inferring a guard
    // pattern from source text.  A zero-token include may be a skipped guard
    // include, but it may also be an empty header, so classify it as unknown
    // guard state unless a future producer record proves the reason.
    identity.kind =
        include.cover.IsValid()
            ? IncludeGuardObservationKind::ActiveIncludeMayMutateGuard
            : IncludeGuardObservationKind::UnknownGuardEffect;
    identity.producerProvenGuard = false;
    return identity;
  };

  auto pragmaClassificationFromText =
      [&](StringRef text) -> PragmaStateClassification {
    std::optional<ParsedDiagnosticPragmaStateDirective> parsed =
        parseDiagnosticPragmaStateDirective(text, lexLang_);
    if (!parsed)
      return PragmaStateClassification::UnknownPragmaState;
    switch (parsed->action) {
    case DiagnosticPragmaStateAction::Setting:
      return PragmaStateClassification::KnownLocalPragmaState;
    case DiagnosticPragmaStateAction::Push:
    case DiagnosticPragmaStateAction::Pop:
      return PragmaStateClassification::KnownBalancedPragmaState;
    }
    llvm_unreachable("Invalid diagnostic pragma action");
  };

  auto pragmaIdentityFromPragma =
      [&](const RefoldModel::PragmaDirective &pragma) -> PragmaStateIdentity {
    PragmaStateIdentity identity;
    identity.pragmaId = pragma.id;
    identity.sitePath = pragma.sitePath.str();
    identity.siteBegin = pragma.siteB;
    identity.siteEnd = pragma.siteE;
    identity.ownerIncludeId = pragma.ownerIncludeId;
    identity.classification = pragmaClassificationFromText(pragma.text);
    // Deterministic textual fingerprint.  This is not a semantic parser; it
    // keeps theorem logs anchored to the producer-supplied directive bytes.
    identity.directiveFingerprint =
        llvm::formatv("{0}:{1}", pragma.text.size(), pragma.text).str();
    return identity;
  };

  auto conditionalSelectedArmCount =
      [](const RefoldModel::CondGroup &group) -> uint64_t {
    uint64_t selectedCount = 0;
    for (const RefoldModel::CondArm &arm : group.arms)
      if (arm.selected)
        ++selectedCount;
    return selectedCount;
  };

  auto conditionalGroupHasUniqueSelectedArm =
      [&](const RefoldModel::CondGroup &group) {
        return conditionalSelectedArmCount(group) == 1;
      };

  auto conditionalArmConditionWasProducerEvaluated =
      [](const RefoldModel::CondGroup &group,
         const RefoldModel::CondArm &queriedArm) {
        // Only conditions reached by the producer's selected branch path are
        // semantic observations.  For an #if/#elif/#else chain, conditions are
        // evaluated until the selected arm is reached.  Later #elif conditions
        // are source text, but they were not queried by the preprocessor and
        // cannot be used as suffix-state proof.
        bool reachedByProducer = true;
        for (const RefoldModel::CondArm &arm : group.arms) {
          if (arm.id == queriedArm.id)
            return reachedByProducer && arm.cond.has_value();
          if (arm.selected)
            reachedByProducer = false;
        }
        return false;
      };

  auto conditionalArmSelectionTruthProducerProven =
      [&](const RefoldModel::CondGroup &group,
          const RefoldModel::CondArm &arm) {
        if (!conditionalGroupHasUniqueSelectedArm(group))
          return false;
        if (arm.selected)
          return true;
        return conditionalArmConditionWasProducerEvaluated(group, arm);
      };

  auto conditionalGroupIdentityFromGroup =
      [&](const RefoldModel::CondGroup &group) -> ConditionalStateIdentity {
    ConditionalStateIdentity identity;
    identity.role = ConditionalStateRole::ConditionalGroup;
    identity.groupId = group.id;
    identity.file = group.file.str();
    identity.groupBegin = group.groupB;
    identity.groupEnd = group.groupE;
    identity.parentArmId = group.parentArmId;
    identity.parentIncludeId = group.parentIncludeId;
    identity.conditionTruthProducerProven =
        conditionalGroupHasUniqueSelectedArm(group);
    return identity;
  };

  auto conditionalArmIdentityFromArm =
      [&](const RefoldModel::CondGroup &group, const RefoldModel::CondArm &arm,
          bool reverseSolvedDirectiveRequired) -> ConditionalStateIdentity {
    ConditionalStateIdentity identity;
    identity.role = arm.selected ? ConditionalStateRole::ActiveArm
                                 : ConditionalStateRole::InactiveArm;
    identity.groupId = group.id;
    identity.armId = arm.id;
    identity.file = group.file.str();
    identity.groupBegin = group.groupB;
    identity.groupEnd = group.groupE;
    identity.parentArmId = group.parentArmId;
    identity.parentIncludeId = group.parentIncludeId;
    identity.armKind = arm.kind.str();
    if (arm.cond)
      identity.conditionText = arm.cond->str();
    identity.selected = arm.selected;
    if (arm.span) {
      identity.aTokenBegin = arm.span->begin;
      identity.aTokenEnd = arm.span->end;
    }
    identity.conditionTruthProducerProven =
        conditionalArmSelectionTruthProducerProven(group, arm);
    identity.reverseSolvedDirectiveRequired = reverseSolvedDirectiveRequired;
    return identity;
  };

  auto recordCounterInvocationEvents =
      [&](const RefoldModel::MacroInvocation &macro) {
        if (macro.name != "__COUNTER__")
          return;

        auditDeltaFact(DirectStateCheckKind::CounterEvent,
                       OwnerStateComponent::Counter,
                       "producer __COUNTER__ invocation events");

        uint64_t ordinal = 0;
        for (const auto &range :
             macroTopology_.CounterOutputRangesForInvocation(macro)) {
          facts.AddCounterEvent(macroTopology_.BuildCounterEventIdentity(
              macro, ordinal++, range.first, range.second));
        }

        if (ordinal == 0)
          markMissingAndUnmodeled(
              MissingStateFactKind::MissingCounterFacts,
              "__COUNTER__ invocation has no producer output range");
      };

  auto scanConditionalMacroState = [&](StringRef conditionText) {
    // distinguishes `defined(NAME)` from ordinary conditional macro
    // dependencies.  This scanner is deliberately conservative and
    // deterministic: it recognizes preprocessor identifiers while skipping
    // string/character literals and comments.  If a `defined` operand is
    // malformed or absent, the owner is marked unmodeled rather than guessed.
    const std::string text = conditionText.str();
    size_t i = 0;

    auto isIdentStart = [](unsigned char c) {
      return std::isalpha(c) || c == '_';
    };
    auto isIdentContinue = [](unsigned char c) {
      return std::isalnum(c) || c == '_';
    };
    auto skipSpace = [&]() {
      while (i < text.size() &&
             std::isspace(static_cast<unsigned char>(text[i])))
        ++i;
    };
    auto skipQuoted = [&](char quote) {
      ++i;
      while (i < text.size()) {
        if (text[i] == '\\') {
          i += std::min<size_t>(2, text.size() - i);
          continue;
        }
        if (text[i++] == quote)
          return;
      }
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingConditionalFacts,
          "unterminated conditional string/character literal");
    };
    auto readIdentifier = [&]() -> std::optional<StringRef> {
      if (i >= text.size() ||
          !isIdentStart(static_cast<unsigned char>(text[i])))
        return std::nullopt;
      const size_t begin = i++;
      while (i < text.size() &&
             isIdentContinue(static_cast<unsigned char>(text[i])))
        ++i;
      return StringRef(text).substr(begin, i - begin);
    };

    while (i < text.size()) {
      const char c = text[i];
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++i;
        continue;
      }
      if (c == '"' || c == '\'') {
        skipQuoted(c);
        continue;
      }
      if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
        break;
      }
      if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
        i += 2;
        const size_t end = text.find("*/", i);
        if (end == std::string::npos) {
          markMissingAndUnmodeled(MissingStateFactKind::MissingConditionalFacts,
                                  "unterminated conditional block comment");
          break;
        }
        i = end + 2;
        continue;
      }

      std::optional<StringRef> identifier = readIdentifier();
      if (!identifier) {
        ++i;
        continue;
      }

      if (*identifier == "defined") {
        skipSpace();
        const bool parenthesized = i < text.size() && text[i] == '(';
        if (parenthesized) {
          ++i;
          skipSpace();
        }
        std::optional<StringRef> operand = readIdentifier();
        if (!operand) {
          markMissingAndUnmodeled(MissingStateFactKind::MissingConditionalFacts,
                                  "malformed defined() operand");
          continue;
        }
        recordMacroObservation(MacroObservationKind::DefinedOperator,
                               identityFromMacroName(*operand));
        if (parenthesized) {
          skipSpace();
          if (i < text.size() && text[i] == ')')
            ++i;
          else
            markMissingAndUnmodeled(
                MissingStateFactKind::MissingConditionalFacts,
                "malformed defined() closing parenthesis");
        }
        continue;
      }

      recordMacroObservation(MacroObservationKind::ConditionalEvaluation,
                             identityFromMacroName(*identifier));
    }
  };

  auto recordMacroDirective =
      [&](const RefoldModel::MacroDirective &directive) {
        const MacroStateIdentity identity = identityFromDirective(directive);
        if (directive.subkind == "#define") {
          auditDeltaFact(DirectStateCheckKind::MacroDefinitionDirective,
                         OwnerStateComponent::MacroState,
                         "#define directive state fact");
          facts.AddMacroDefinition(identity);
        } else if (directive.subkind == "#undef") {
          auditDeltaFact(DirectStateCheckKind::MacroUndefDirective,
                         OwnerStateComponent::MacroState,
                         "#undef directive state fact");
          facts.AddMacroUndefinition(identity);
        } else
          markMissingAndUnmodeled(MissingStateFactKind::MissingMacroFacts,
                                  "unknown macro directive kind");

        if (directive.name.empty() || directive.text.empty())
          markMissingAndUnmodeled(MissingStateFactKind::MissingMacroFacts,
                                  "macro directive missing name or text");
      };

  auto recordPragma = [&](const RefoldModel::PragmaDirective &pragma) {
    // record a component-specific pragma event.  Unknown pragmas still
    // fail closed through the event classification, while known diagnostic
    // pragma state can later be discharged by balanced/local pragma proofs.
    auditDeltaFact(DirectStateCheckKind::PragmaDirective,
                   OwnerStateComponent::PragmaState,
                   "#pragma directive state fact");
    const PragmaStateIdentity identity = pragmaIdentityFromPragma(pragma);
    facts.AddPragmaStateEvent(identity);
    if (identity.classification ==
        PragmaStateClassification::UnknownPragmaState)
      markMissing(MissingStateFactKind::MissingPragmaFacts,
                  "unknown pragma semantics");
  };

  auto recordLineControl = [&](const RefoldModel::LineControlEvent &event) {
    // model #line as a zero-token state mutation whose operands were evaluated
    // by the producer.  The component-specific event identity is the only
    // line-control mutation fact consumed by theorem-facing code.
    auditDeltaFact(DirectStateCheckKind::LineControlDirective,
                   OwnerStateComponent::LineNumber,
                   "#line directive line-number state fact");
    auditDeltaFact(DirectStateCheckKind::LineControlDirective,
                   OwnerStateComponent::FileState,
                   "#line directive file-state fact");
    auditDeltaFact(DirectStateCheckKind::LineControlDirective,
                   OwnerStateComponent::FileName,
                   "#line directive file-name state fact");
    const LineControlStateIdentity identity =
        lineControlIdentityFromEvent(event);
    facts.AddLineControlEvent(identity);
    if (!event.active || !event.producerProven) {
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingLineControlFacts,
          "line-control event inactive or not producer-proven");
    }
    if (event.text.empty() || !event.siteB || !event.siteE) {
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingLineControlFacts,
          "line-control event missing directive text or source span");
    }
  };

  auto recordIncludeTransition = [&](const RefoldModel::IncludeItem &include) {
    // include identity and include-guard state are recorded as separate
    // component facts.  The guard macro remains absent unless the producer
    // supplies a dedicated guard oracle; this is deliberately conservative and
    // avoids pattern-matching header guards in the consumer.
    auditDeltaFact(DirectStateCheckKind::IncludeDirectiveState,
                   OwnerStateComponent::IncludeState,
                   "#include directive state fact");
    auditDeltaFact(DirectStateCheckKind::IncludeGuardState,
                   OwnerStateComponent::IncludeGuardState,
                   "include-guard state fact");
    facts.AddIncludeStateEvent(includeStateIdentityFromInclude(include));
    facts.AddIncludeGuardStateEvent(includeGuardIdentityFromInclude(include));
    if (!include.resolvedPath) {
      markMissing(MissingStateFactKind::MissingOwnerOrderingFacts,
                  "include directive missing resolved file identity");
    }
    if (!include.cover.IsValid()) {
      markMissing(MissingStateFactKind::MissingIncludeGuardFacts,
                  "include has no producer token cover; guard skip versus "
                  "empty header is unknown");
    }
  };

  auto recordConditionalArm = [&](const RefoldModel::CondGroup &group,
                                  const RefoldModel::CondArm &arm,
                                  bool requireActiveOwner) {
    // Record branch-selection state from the producer's active path.  A repair
    // may use a conditional arm as a source witness only when that arm is the
    // producer-selected arm; inactive-arm repair would reverse-solve
    // source-only conditional text from downstream B tokens and therefore
    // remains outside theorem authority.
    auditDeltaFact(DirectStateCheckKind::ConditionalDirectiveState,
                   OwnerStateComponent::ConditionalState,
                   "conditional arm state fact");

    const bool reverseSolvedDirectiveRequired =
        requireActiveOwner && !arm.selected;
    facts.AddConditionalStateEvent(conditionalArmIdentityFromArm(
        group, arm, reverseSolvedDirectiveRequired));

    if (!conditionalArmSelectionTruthProducerProven(group, arm)) {
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingConditionalFacts,
          "conditional arm selection was not producer-proven");
    }
    if (reverseSolvedDirectiveRequired) {
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingConditionalFacts,
          "inactive conditional arm cannot serve as active source witness");
    }

    if (arm.cond && conditionalArmConditionWasProducerEvaluated(group, arm)) {
      scanTextForBuiltins(*arm.cond);
      scanConditionalMacroState(*arm.cond);
    }
  };

  auto recordConditionalGroup = [&](const RefoldModel::CondGroup &group) {
    // A conditional group is the zero-token state owner for branch selection:
    // the directive island observes macro/conditional state, selects one arm,
    // and mutates the conditional-state context seen by nested owners. This
    // records only the arm predicates the producer actually evaluated; #elif
    // conditions after the selected arm are source text but not state reads.
    auditDeltaFact(DirectStateCheckKind::ConditionalDirectiveState,
                   OwnerStateComponent::ConditionalState,
                   "conditional group state fact");
    facts.AddConditionalStateEvent(conditionalGroupIdentityFromGroup(group));

    const bool uniqueSelectedArm = conditionalGroupHasUniqueSelectedArm(group);
    if (!uniqueSelectedArm) {
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingConditionalFacts,
          "conditional group does not have exactly one producer-selected arm");
    }

    bool reachedSelectedArm = false;
    for (const RefoldModel::CondArm &arm : group.arms) {
      const bool reachedByProducer = !reachedSelectedArm;
      if (!uniqueSelectedArm || reachedByProducer || arm.selected)
        recordConditionalArm(group, arm, /*requireActiveOwner=*/false);
      if (arm.selected)
        reachedSelectedArm = true;
    }
  };

  auto recordMacroInvocation = [&](const RefoldModel::MacroInvocation &macro) {
    const MacroStateIdentity identity = identityFromInvocation(macro);
    facts.AddMacroRequirement(identity);
    recordMacroObservation(MacroObservationKind::Expansion, identity);
    recordCounterInvocationEvents(macro);
    if (macro.name.empty() ||
        (macro.subkind != "obj" && macro.subkind != "func"))
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingMacroFacts,
          "macro invocation missing name or recognized kind");
    if (macro.subkind == "func" && !macro.definitionDirectiveId)
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingMacroFacts,
          "function-like macro invocation missing definition id");

    scanTextForBuiltins(macro.name);
    if (macro.invText)
      scanTextForBuiltins(*macro.invText);
    else if (macro.subkind == "func")
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingMacroFacts,
          "function-like macro invocation missing invocation text");

    for (const RefoldModel::PPSpan &span : macro.spans)
      scanPPSpanForBuiltins(span);
    for (const RefoldModel::PPArgSpan &span : macro.argSpans)
      scanPPSpanForBuiltins(span);
    for (const RefoldModel::PPArgSpan &span : macro.stringifySpans)
      scanPPSpanForBuiltins(span);
    for (const RefoldModel::PPArgSpan &span : macro.pasteSpans)
      scanPPSpanForBuiltins(span);
    for (const RefoldModel::PPSpan &span : macro.bodySpans)
      scanPPSpanForBuiltins(span);
  };

  const OwnerStateFactIndex &stateIndex = GetOwnerStateFactIndex();

  if (owner.IsMacroDirective()) {
    const RefoldModel::MacroDirective *directive = nullptr;
    if (owner.macroDirectiveId) {
      auto it = stateIndex.macroDirectiveById.find(*owner.macroDirectiveId);
      if (it != stateIndex.macroDirectiveById.end())
        directive = it->second;
    }
    if (directive)
      recordMacroDirective(*directive);
    else
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingMacroFacts,
          "macro directive owner id not found in producer map");
    return finalize();
  }

  if (owner.IsLineControlIsland()) {
    const RefoldModel::LineControlEvent *event = nullptr;
    if (owner.lineControlId) {
      auto it = stateIndex.lineControlById.find(*owner.lineControlId);
      if (it != stateIndex.lineControlById.end())
        event = it->second;
    }
    if (event)
      recordLineControl(*event);
    else
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingLineControlFacts,
          "line-control owner id not found in producer map");
    return finalize();
  }

  if (owner.IsPragmaIsland()) {
    const RefoldModel::PragmaDirective *pragma = nullptr;
    if (owner.pragmaId) {
      auto it = stateIndex.pragmaById.find(*owner.pragmaId);
      if (it != stateIndex.pragmaById.end())
        pragma = it->second;
    }
    if (pragma)
      recordPragma(*pragma);
    else
      markMissingAndUnmodeled(MissingStateFactKind::MissingPragmaFacts,
                              "pragma owner id not found in producer map");
    return finalize();
  }

  if (owner.IsMacroInvocation()) {
    const RefoldModel::MacroInvocation *macro = nullptr;
    if (owner.macroInvocationId) {
      auto it = stateIndex.macroInvocationById.find(*owner.macroInvocationId);
      if (it != stateIndex.macroInvocationById.end())
        macro = it->second;
    }
    if (macro)
      recordMacroInvocation(*macro);
    else
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingMacroFacts,
          "macro invocation owner id not found in producer map");
    return finalize();
  }

  if (owner.IsConditionalArm()) {
    if (owner.condArmId) {
      if (const std::optional<RefoldModel::ArmRef> armRef =
              model_.GetArmRefById(*owner.condArmId)) {
        recordConditionalArm(*armRef->group, *armRef->arm,
                             /*requireActiveOwner=*/true);
      } else {
        markMissingAndUnmodeled(
            MissingStateFactKind::MissingConditionalFacts,
            "conditional arm owner id not found in producer map");
      }
    } else {
      markMissingAndUnmodeled(MissingStateFactKind::MissingConditionalFacts,
                              "conditional arm owner missing arm id");
    }
  }

  if (owner.IsConditionalGroup()) {
    bool found = false;
    if (owner.condGroupId) {
      if (const RefoldModel::CondGroup *group =
              model_.GetCondGroupById(*owner.condGroupId)) {
        recordConditionalGroup(*group);
        found = true;
      }
    }
    if (!found)
      markMissingAndUnmodeled(
          MissingStateFactKind::MissingConditionalFacts,
          "conditional group owner id not found in producer map");
    return finalize();
  }

  bool exactIncludeOwnerFound = false;
  if (owner.IsInclude() && owner.includeId) {
    if (const RefoldModel::IncludeItem *include =
            model_.GetIncludeById(*owner.includeId)) {
      recordIncludeTransition(*include);
      exactIncludeOwnerFound = true;
    }
  }

  const std::optional<uint64_t> sourceBucket = OwnerSourceBucketKey(owner);

  auto lookupBucket = [](const auto &map, uint64_t key) {
    using MapT = std::decay_t<decltype(map)>;
    using BucketT = typename MapT::mapped_type;
    using ValueT = typename BucketT::value_type;
    auto it = map.find(key);
    if (it == map.end())
      return ArrayRef<ValueT>();
    return ArrayRef<ValueT>(it->second);
  };

  if (sourceBucket) {
    // All following scans are owner-local bucket scans.  The final
    // OwnerMatchesSourceSite() predicate is intentionally retained as the
    // theorem-facing admission gate; the index only removes irrelevant producer
    // facts before applying the existing proof predicate.
    for (const RefoldModel::IncludeItem *include :
         lookupBucket(stateIndex.includesByParentIncludeKey, *sourceBucket)) {
      if (OwnerMatchesSourceSite(owner, include->sitePath, include->parent,
                                 include->siteB, include->siteE))
        recordIncludeTransition(*include);
    }

    for (const RefoldModel::MacroDirective *directive : lookupBucket(
             stateIndex.macroDirectivesByOwnerIncludeKey, *sourceBucket)) {
      if (OwnerMatchesSourceSite(owner, directive->sitePath,
                                 directive->ownerIncludeId, directive->siteB,
                                 directive->siteE))
        recordMacroDirective(*directive);
    }

    for (const RefoldModel::LineControlEvent *event : lookupBucket(
             stateIndex.lineControlsByOwnerIncludeKey, *sourceBucket)) {
      if (event->siteB && event->siteE) {
        if (OwnerMatchesSourceSite(owner, event->physicalFile,
                                   event->ownerIncludeId, *event->siteB,
                                   *event->siteE))
          recordLineControl(*event);
        continue;
      }

      // Older maps may identify only the owning include instance for a line
      // control event.  The owner-local index already selected the only
      // candidate include bucket, but keep the original explicit predicate so
      // missing source spans remain a named conservative fact.
      const bool ownerMatchesByInclude =
          (owner.IsTU() && !event->ownerIncludeId) ||
          (owner.IsInclude() && owner.includeId && event->ownerIncludeId &&
           *owner.includeId == *event->ownerIncludeId);
      if (ownerMatchesByInclude) {
        recordLineControl(*event);
        markMissingAndUnmodeled(
            MissingStateFactKind::MissingLineControlFacts,
            "line-control event matched by owner but lacks source span");
      }
    }

    for (const RefoldModel::PragmaDirective *pragma :
         lookupBucket(stateIndex.pragmasByOwnerIncludeKey, *sourceBucket)) {
      std::optional<uint64_t> pragmaOwnerIncludeId = pragma->ownerIncludeId;
      if (!pragmaOwnerIncludeId) {
        // The index uses this same recovery to choose the bucket.  Recompute
        // the value here only for the final OwnerMatchesSourceSite() proof
        // predicate so the delta semantics match the canonical owner-site
        // recovery.
        for (const RefoldModel::Segment &segment :
             model_.GetSegmentsForFile(pragma->sitePath)) {
          if (segment.b <= pragma->siteB && pragma->siteE <= segment.e) {
            pragmaOwnerIncludeId = segment.ownerIncludeId;
            break;
          }
        }
      }
      const bool pragmaBelongs =
          OwnerMatchesSourceSite(owner, pragma->sitePath, pragmaOwnerIncludeId,
                                 pragma->siteB, pragma->siteE);
      if (pragmaBelongs)
        recordPragma(*pragma);
    }

    for (const RefoldModel::CondGroup *group :
         lookupBucket(stateIndex.condGroupsByParentIncludeKey, *sourceBucket)) {
      if (!OwnerMatchesSourceSite(owner, group->file, group->parentIncludeId,
                                  group->groupB, group->groupE))
        continue;
      recordConditionalGroup(*group);
    }

    for (const RefoldModel::MacroInvocation *macro : lookupBucket(
             stateIndex.macroInvocationsByOwnerIncludeKey, *sourceBucket)) {
      if (macro->invFile && macro->invB && macro->invE &&
          OwnerMatchesSourceSite(owner, *macro->invFile, macro->ownerIncludeId,
                                 *macro->invB, *macro->invE))
        recordMacroInvocation(*macro);
    }
  }

  if (owner.IsInclude() && owner.includeId && !exactIncludeOwnerFound)
    markMissingAndUnmodeled(MissingStateFactKind::MissingOwnerOrderingFacts,
                            "include owner id not found in producer map");

  return finalize();
}

bool RefoldOwnerStateProof::OwnerObserverSummaryObservesComponent(
    const OwnerObserverSummary &summary, OwnerStateComponent component) {
  switch (component) {
  case OwnerStateComponent::MacroState:
    // A macro-state mutation can be observed either by ordinary expansion,
    // by a `defined` query, or by conditional evaluation that depends on macro
    // definitions.  Keeping these under the macro-state umbrella lets the state
    // gateway ask one question for #define/#undef stability while still
    // preserving the more precise observer kind in each site.
    return summary.observesMacroExpansion || summary.observesDefinedOperator ||
           summary.observesConditionalEvaluation;
  case OwnerStateComponent::DefinedOperator:
    return summary.observesDefinedOperator;
  case OwnerStateComponent::ConditionalState:
    return summary.observesConditionalEvaluation;
  case OwnerStateComponent::LineNumber:
    return summary.observesLineNumber;
  case OwnerStateComponent::FileState:
    return summary.observesFileState;
  case OwnerStateComponent::FileName:
    return summary.observesFileName;
  case OwnerStateComponent::Counter:
    return summary.observesCounter;
  case OwnerStateComponent::PragmaState:
    return summary.observesPragmaState;
  case OwnerStateComponent::IncludeGuardState:
    return summary.observesIncludeGuardState;
  case OwnerStateComponent::IncludeState:
    return summary.observesIncludeState;
  case OwnerStateComponent::UnmodeledState:
  case OwnerStateComponent::Unknown:
    return false;
  }
  llvm_unreachable("Invalid owner state component");
}

SuffixObservationKind RefoldOwnerStateProof::ObservationKindForComponent(
    OwnerStateComponent component) {
  switch (component) {
  case OwnerStateComponent::MacroState:
    return SuffixObservationKind::MacroExpansionObservation;
  case OwnerStateComponent::DefinedOperator:
    return SuffixObservationKind::DefinedOperatorObservation;
  case OwnerStateComponent::ConditionalState:
    return SuffixObservationKind::ConditionalMacroObservation;
  case OwnerStateComponent::LineNumber:
    return SuffixObservationKind::LineStateObservation;
  case OwnerStateComponent::FileState:
    return SuffixObservationKind::FileStateObservation;
  case OwnerStateComponent::FileName:
    return SuffixObservationKind::FileNameStateObservation;
  case OwnerStateComponent::Counter:
    return SuffixObservationKind::CounterObservation;
  case OwnerStateComponent::PragmaState:
    return SuffixObservationKind::PragmaStateObservation;
  case OwnerStateComponent::IncludeGuardState:
    return SuffixObservationKind::IncludeGuardStateObservation;
  case OwnerStateComponent::IncludeState:
    return SuffixObservationKind::IncludeStateObservation;
  case OwnerStateComponent::UnmodeledState:
    return SuffixObservationKind::UnmodeledStateObservation;
  case OwnerStateComponent::Unknown:
    return SuffixObservationKind::Unknown;
  }
  llvm_unreachable("Invalid owner state component");
}

ArrayRef<uint64_t> RefoldOwnerStateProof::ObserverSiteIndexesForComponent(
    const OwnerStateGraph &graph, OwnerStateComponent component) {
  static const std::vector<uint64_t> empty;
  switch (component) {
  case OwnerStateComponent::MacroState:
    return graph.observerIndex.macroStateObservers;
  case OwnerStateComponent::DefinedOperator:
    return graph.observerIndex.definedOperatorObservers;
  case OwnerStateComponent::ConditionalState:
    return graph.observerIndex.conditionalStateObservers;
  case OwnerStateComponent::LineNumber:
    return graph.observerIndex.lineStateObservers;
  case OwnerStateComponent::FileState:
    return graph.observerIndex.fileStateObservers;
  case OwnerStateComponent::FileName:
    return graph.observerIndex.fileNameStateObservers;
  case OwnerStateComponent::Counter:
    return graph.observerIndex.counterObservers;
  case OwnerStateComponent::PragmaState:
    return graph.observerIndex.pragmaStateObservers;
  case OwnerStateComponent::IncludeGuardState:
    return graph.observerIndex.includeGuardStateObservers;
  case OwnerStateComponent::IncludeState:
    return graph.observerIndex.includeStateObservers;
  case OwnerStateComponent::UnmodeledState:
    return graph.observerIndex.unmodeledStateObservers;
  case OwnerStateComponent::Unknown:
    return empty;
  }
  llvm_unreachable("Invalid owner state component");
}

OwnerStateGraph RefoldOwnerStateProof::BuildOwnerStateGraph() const {
  OwnerStateGraph graph;
  const bool auditDirectState = theoremAudit_.IsNoLegacyAuditEnabled();

  auto spanCover = [](ArrayRef<RefoldModel::PPSpan> spans) -> OwnerTokenRange {
    uint64_t begin = std::numeric_limits<uint64_t>::max();
    uint64_t end = 0;
    bool saw = false;
    for (const RefoldModel::PPSpan &span : spans) {
      if (!span.IsValid())
        continue;
      begin = std::min<uint64_t>(begin, span.begin);
      end = std::max<uint64_t>(end, span.end);
      saw = true;
    }
    if (!saw)
      return OwnerTokenRange::From(0, 0);
    return OwnerTokenRange::From(begin, end);
  };

  auto segmentOwnerForRange = [&](StringRef file, uint64_t begin, uint64_t end)
      -> std::optional<
          std::pair<std::optional<uint64_t>, std::optional<uint64_t>>> {
    ArrayRef<RefoldModel::Segment> segments = model_.GetSegmentsForFile(file);
    if (segments.empty())
      return std::nullopt;

    uint64_t cursor = begin;
    bool saw = false;
    std::optional<uint64_t> includeId;
    std::optional<uint64_t> condArmId;
    for (const RefoldModel::Segment &segment : segments) {
      if (segment.e <= begin)
        continue;
      if (end <= segment.b)
        break;
      if (!intervalsOverlap(begin, end, segment.b, segment.e))
        continue;
      const uint64_t partBegin = std::max<uint64_t>(begin, segment.b);
      const uint64_t partEnd = std::min<uint64_t>(end, segment.e);
      if (partBegin > cursor)
        return std::nullopt;
      if (!saw) {
        includeId = segment.ownerIncludeId;
        condArmId = segment.ownerCondArmId;
        saw = true;
      } else if (includeId != segment.ownerIncludeId ||
                 condArmId != segment.ownerCondArmId) {
        return std::nullopt;
      }
      cursor = partEnd;
    }
    if (!saw || cursor < end)
      return std::nullopt;
    return std::make_pair(includeId, condArmId);
  };

  auto enclosingCondArmId =
      [&](StringRef file, std::optional<uint64_t> ownerIncludeId,
          uint64_t begin, uint64_t end) -> std::optional<uint64_t> {
    const std::optional<RefoldModel::ArmRef> armRef =
        model_.FindArmRefForByte(file, ownerIncludeId, begin);
    if (!armRef || !armRef->arm)
      return std::nullopt;
    if (!macroTopology_.SourceRangeInsideConditionalArm(
            armRef->arm->id, file, ownerIncludeId, begin, end))
      return std::nullopt;
    return armRef->arm->id;
  };

  auto addNode = [&](OwnerStateGraphNodeKind kind, Owner owner,
                     OwnerSourceRange source, OwnerTokenRange aTokens,
                     StringRef detail) {
    // is a census, not an admission gate.  Unknown or source-less
    // owners cannot be ordered against a source boundary.  Token-only ordinary
    // owners may still be added by later proof passes, but the current producer
    // map gives every directive/event node a source anchor, so missing source
    // facts are represented by the component-specific missing-state markers
    // instead of manufacturing a false order.
    if (!owner.IsKnown() || !source.IsComplete())
      return;

    OwnerStateGraphNode node;
    node.kind = kind;
    node.closure = AttachCanonicalStateSummary(
        OwnerClosure::From(std::move(owner), std::move(source), aTokens,
                           OwnerTokenRange::From(0, 0)));
    node.state = node.closure.stateOut;
    node.source = node.closure.source;
    node.aTokens = node.closure.aTokens;
    node.containingIncludeId = node.source.includeId;
    node.containingConditionalArmId = node.closure.owner.condArmId;
    node.containingMacroInvocationId = node.closure.owner.macroInvocationId;
    node.detail = detail.str();

    const DirectStateCheckKind directKind =
        DirectStateCheckKindForGraphNode(kind);
    if (auditDirectState && kind == OwnerStateGraphNodeKind::LineControlEvent) {
      AuditDirectStateCheckClosure(
          DirectStateCheckKind::LineControlDirective,
          OwnerStateComponent::LineNumber,
          DirectStateCheckClosureKind::OwnerStateGraphEdge,
          StateMutationKind::Unknown, "owner-state-graph", detail);
      AuditDirectStateCheckClosure(
          DirectStateCheckKind::LineControlDirective,
          OwnerStateComponent::FileState,
          DirectStateCheckClosureKind::OwnerStateGraphEdge,
          StateMutationKind::Unknown, "owner-state-graph", detail);
      AuditDirectStateCheckClosure(
          DirectStateCheckKind::LineControlDirective,
          OwnerStateComponent::FileName,
          DirectStateCheckClosureKind::OwnerStateGraphEdge,
          StateMutationKind::Unknown, "owner-state-graph", detail);
    } else if (auditDirectState &&
               directKind != DirectStateCheckKind::Unknown) {
      AuditDirectStateCheckClosure(
          directKind, DirectStateComponentForGraphNode(kind),
          DirectStateCheckClosureKind::OwnerStateGraphEdge,
          StateMutationKind::Unknown, "owner-state-graph", detail);
    }

    graph.nodes.push_back(std::move(node));
  };

  for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
    const std::optional<uint64_t> condArmId = enclosingCondArmId(
        include.sitePath, include.parent, include.siteB, include.siteE);
    const OwnerTokenRange cover = spanCover(include.spans);
    const Owner owner = Owner::Include(include.id, condArmId);

    // Include entry and exit are explicit zero-token graph nodes. They bracket
    // the included token materialization in preprocessing order and remain
    // comparable by source site even when an include expands to no A tokens
    // because of include guards or an empty header.
    addNode(OwnerStateGraphNodeKind::IncludeEntry, owner,
            OwnerSourceRange::From(include.sitePath, include.siteB,
                                   include.siteB, include.parent),
            OwnerTokenRange::From(cover.begin, cover.begin),
            formatv("include {0} entry", include.id).str());
    addNode(OwnerStateGraphNodeKind::IncludeExit, owner,
            OwnerSourceRange::From(include.sitePath, include.siteE,
                                   include.siteE, include.parent),
            OwnerTokenRange::From(cover.end, cover.end),
            formatv("include {0} exit", include.id).str());
  }

  for (const RefoldModel::MacroDirective &directive :
       model_.GetMacroDirectives()) {
    if (IsVirtualInitialMacroDirectiveSource(directive.sitePath))
      continue;

    const std::optional<uint64_t> condArmId =
        enclosingCondArmId(directive.sitePath, directive.ownerIncludeId,
                           directive.siteB, directive.siteE);
    // #define/#undef directives are zero-token macro-state events
    // when they have a real source-order anchor.  Clang's virtual <built-in>
    // macro definitions seed the initial environment before source processing
    // and are deliberately skipped above; they cannot be suffix observers after
    // a source edit boundary.
    addNode(directive.subkind == "#define"
                ? OwnerStateGraphNodeKind::MacroDefineEvent
            : directive.subkind == "#undef"
                ? OwnerStateGraphNodeKind::MacroUndefEvent
                : OwnerStateGraphNodeKind::Unknown,
            Owner::MacroDirective(directive.id, condArmId),
            OwnerSourceRange::From(directive.sitePath, directive.siteB,
                                   directive.siteE, directive.ownerIncludeId),
            spanCover(directive.spans),
            formatv("macro directive {0} {1}", directive.id, directive.subkind)
                .str());
  }

  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    if (!event.siteB || !event.siteE)
      continue;
    const std::optional<uint64_t> condArmId = enclosingCondArmId(
        event.physicalFile, event.ownerIncludeId, *event.siteB, *event.siteE);
    addNode(OwnerStateGraphNodeKind::LineControlEvent,
            Owner::LineControlIsland(event.id, condArmId),
            OwnerSourceRange::From(event.physicalFile, *event.siteB,
                                   *event.siteE, event.ownerIncludeId),
            OwnerTokenRange::From(0, 0),
            formatv("line-control event {0}", event.id).str());
  }

  for (const RefoldModel::PragmaDirective &pragma : model_.GetPragmas()) {
    std::optional<uint64_t> ownerIncludeId = pragma.ownerIncludeId;
    std::optional<uint64_t> condArmId;
    if (auto owner =
            segmentOwnerForRange(pragma.sitePath, pragma.siteB, pragma.siteE)) {
      if (!ownerIncludeId)
        ownerIncludeId = owner->first;
      condArmId = owner->second;
    }
    if (!condArmId)
      condArmId = enclosingCondArmId(pragma.sitePath, ownerIncludeId,
                                     pragma.siteB, pragma.siteE);
    addNode(OwnerStateGraphNodeKind::PragmaEvent,
            Owner::PragmaIsland(pragma.id, condArmId),
            OwnerSourceRange::From(pragma.sitePath, pragma.siteB, pragma.siteE,
                                   ownerIncludeId),
            OwnerTokenRange::From(0, 0),
            formatv("pragma event {0}", pragma.id).str());
  }

  for (const RefoldModel::CondGroup &group : model_.GetConds()) {
    addNode(OwnerStateGraphNodeKind::ConditionalEvent,
            Owner::ConditionalGroup(group.id),
            OwnerSourceRange::From(group.file, group.groupB, group.groupE,
                                   group.parentIncludeId),
            OwnerTokenRange::From(0, 0),
            formatv("conditional group {0}", group.id).str());
    for (const RefoldModel::CondArm &arm : group.arms) {
      OwnerTokenRange tokens = OwnerTokenRange::From(0, 0);
      if (arm.span && arm.span->IsValid())
        tokens = OwnerTokenRange::From(arm.span->begin, arm.span->end);
      addNode(OwnerStateGraphNodeKind::ConditionalEvent,
              Owner::ConditionalArm(arm.id),
              OwnerSourceRange::From(group.file, arm.bodyB, arm.bodyE,
                                     group.parentIncludeId),
              tokens, formatv("conditional arm {0}", arm.id).str());
    }
  }

  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (!macro.invFile || !macro.invB || !macro.invE)
      continue;
    const std::optional<uint64_t> condArmId = enclosingCondArmId(
        *macro.invFile, macro.ownerIncludeId, *macro.invB, *macro.invE);
    OwnerTokenRange tokens = OwnerTokenRange::From(0, 0);
    if (macro.cover.IsValid())
      tokens = OwnerTokenRange::From(macro.cover.begin, macro.cover.end);
    addNode(macro.callerMacroId ? OwnerStateGraphNodeKind::NestedMacroExpansion
                                : OwnerStateGraphNodeKind::MacroInvocation,
            Owner::MacroInvocation(macro.id, condArmId),
            OwnerSourceRange::From(*macro.invFile, *macro.invB, *macro.invE,
                                   macro.ownerIncludeId),
            tokens, formatv("macro invocation {0}", macro.id).str());
  }

  auto ownerIdentityKey = [](const Owner &owner)
      -> std::tuple<unsigned, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t> {
    const uint64_t none = std::numeric_limits<uint64_t>::max();
    return std::make_tuple(
        static_cast<unsigned>(owner.kind), owner.includeId.value_or(none),
        owner.macroInvocationId.value_or(none),
        owner.macroDirectiveId.value_or(none),
        owner.lineControlId.value_or(none), owner.pragmaId.value_or(none),
        owner.condGroupId.value_or(none), owner.condArmId.value_or(none));
  };

  auto sourceDomainKey = [](const OwnerSourceRange &source)
      -> std::tuple<std::string, uint64_t, uint64_t, uint64_t> {
    const uint64_t none = std::numeric_limits<uint64_t>::max();
    return std::make_tuple(source.path, source.includeId.value_or(none),
                           source.begin, source.end);
  };

  auto graphNodeLess = [&](const OwnerStateGraphNode &lhs,
                           const OwnerStateGraphNode &rhs) {
    const bool sameSourceDomain = lhs.source.path == rhs.source.path &&
                                  lhs.source.includeId == rhs.source.includeId;
    if (sameSourceDomain) {
      // zero-token directives are ordered against token-producing
      // owners by their physical source anchor when both events live in the
      // same source/include domain.  We deliberately do this before consulting
      // token anchors so #define/#undef/#line/#pragma events cannot all drift
      // to EOF.
      if (lhs.source.begin != rhs.source.begin)
        return lhs.source.begin < rhs.source.begin;
      if (lhs.source.end != rhs.source.end)
        return lhs.source.end < rhs.source.end;
    }

    const bool lhsHasTokens = lhs.HasTokenAnchor();
    const bool rhsHasTokens = rhs.HasTokenAnchor();
    if (lhsHasTokens && rhsHasTokens && lhs.aTokens.begin != rhs.aTokens.begin)
      return lhs.aTokens.begin < rhs.aTokens.begin;
    if (lhsHasTokens && rhsHasTokens && lhs.aTokens.end != rhs.aTokens.end)
      return lhs.aTokens.end < rhs.aTokens.end;
    if (lhsHasTokens != rhsHasTokens)
      return lhsHasTokens;

    const auto lhsSource = sourceDomainKey(lhs.source);
    const auto rhsSource = sourceDomainKey(rhs.source);
    if (lhsSource != rhsSource)
      return lhsSource < rhsSource;
    if (lhs.kind != rhs.kind)
      return static_cast<unsigned>(lhs.kind) < static_cast<unsigned>(rhs.kind);
    return ownerIdentityKey(lhs.closure.owner) <
           ownerIdentityKey(rhs.closure.owner);
  };

  static constexpr OwnerStateComponent components[] = {
      OwnerStateComponent::MacroState,
      OwnerStateComponent::DefinedOperator,
      OwnerStateComponent::ConditionalState,
      OwnerStateComponent::LineNumber,
      OwnerStateComponent::FileState,
      OwnerStateComponent::FileName,
      OwnerStateComponent::Counter,
      OwnerStateComponent::PragmaState,
      OwnerStateComponent::IncludeGuardState,
      OwnerStateComponent::IncludeState};

  auto assignGraphOrder = [&]() {
    std::stable_sort(graph.nodes.begin(), graph.nodes.end(), graphNodeLess);
    for (uint64_t i = 0, e = graph.nodes.size(); i < e; ++i) {
      OwnerStateGraphNode &node = graph.nodes[static_cast<size_t>(i)];
      node.id = i;
      node.order = i;
      node.predecessor = i > 0 ? std::optional<uint64_t>(i - 1) : std::nullopt;
      node.successor =
          i + 1 < e ? std::optional<uint64_t>(i + 1) : std::nullopt;
    }
  };

  auto appendUniqueIndex = [](std::vector<uint64_t> &dst, uint64_t value) {
    if (std::find(dst.begin(), dst.end(), value) == dst.end())
      dst.push_back(value);
  };

  auto appendKeyedIndex = [&](llvm::StringMap<std::vector<uint64_t>> &map,
                              StringRef key, uint64_t value) {
    if (key.empty())
      return;
    appendUniqueIndex(map[key], value);
  };

  auto counterKey = [](const CounterEventIdentity &identity) {
    return formatv("macro={0}:ordinal={1}:atok=[{2},{3})",
                   identity.macroInvocationId, identity.occurrenceOrdinal,
                   identity.aTokenBegin, identity.aTokenEnd)
        .str();
  };

  auto includeKey = [](const IncludeStateIdentity &identity) {
    if (identity.resolvedPath && !identity.resolvedPath->empty())
      return formatv("resolved={0}", *identity.resolvedPath).str();
    if (!identity.target.empty())
      return formatv("target={0}", identity.target).str();
    return formatv("include={0}", identity.includeId).str();
  };

  auto includeGuardKey = [](const IncludeGuardStateIdentity &identity) {
    if (identity.guardMacroName && !identity.guardMacroName->empty())
      return formatv("guard={0}", *identity.guardMacroName).str();
    if (!identity.headerPath.empty())
      return formatv("header={0}", identity.headerPath).str();
    return formatv("include={0}", identity.includeId).str();
  };

  auto pragmaKey = [](const PragmaStateIdentity &identity) {
    return formatv("pragma={0}:class={1}", identity.pragmaId,
                   identity.classification)
        .str();
  };

  auto conditionalKey = [](const ConditionalStateIdentity &identity) {
    if (identity.armId)
      return formatv("group={0}:arm={1}:role={2}", identity.groupId,
                     *identity.armId, identity.role)
          .str();
    return formatv("group={0}:role={1}", identity.groupId, identity.role).str();
  };

  auto recordComponentIndex = [&](OwnerStateComponent component,
                                  uint64_t siteIndex) {
    switch (component) {
    case OwnerStateComponent::MacroState:
      appendUniqueIndex(graph.observerIndex.macroStateObservers, siteIndex);
      return;
    case OwnerStateComponent::DefinedOperator:
      appendUniqueIndex(graph.observerIndex.definedOperatorObservers,
                        siteIndex);
      return;
    case OwnerStateComponent::ConditionalState:
      appendUniqueIndex(graph.observerIndex.conditionalStateObservers,
                        siteIndex);
      return;
    case OwnerStateComponent::LineNumber:
      appendUniqueIndex(graph.observerIndex.lineStateObservers, siteIndex);
      return;
    case OwnerStateComponent::FileState:
      appendUniqueIndex(graph.observerIndex.fileStateObservers, siteIndex);
      return;
    case OwnerStateComponent::FileName:
      appendUniqueIndex(graph.observerIndex.fileNameStateObservers, siteIndex);
      return;
    case OwnerStateComponent::Counter:
      appendUniqueIndex(graph.observerIndex.counterObservers, siteIndex);
      return;
    case OwnerStateComponent::PragmaState:
      appendUniqueIndex(graph.observerIndex.pragmaStateObservers, siteIndex);
      return;
    case OwnerStateComponent::IncludeGuardState:
      appendUniqueIndex(graph.observerIndex.includeGuardStateObservers,
                        siteIndex);
      return;
    case OwnerStateComponent::IncludeState:
      appendUniqueIndex(graph.observerIndex.includeStateObservers, siteIndex);
      return;
    case OwnerStateComponent::UnmodeledState:
      appendUniqueIndex(graph.observerIndex.unmodeledStateObservers, siteIndex);
      return;
    case OwnerStateComponent::Unknown:
      return;
    }
    llvm_unreachable("Invalid owner state component");
  };

  auto recordKeyedIndexes = [&](OwnerStateComponent component,
                                const OwnerStateDelta &delta,
                                uint64_t siteIndex) {
    auto recordMacroName = [&](const MacroStateIdentity &identity) {
      appendKeyedIndex(graph.observerIndex.observersByMacroName,
                       identity.macroName, siteIndex);
    };
    auto recordMacroObservation =
        [&](const MacroStateObservation &observation) {
          recordMacroName(observation.identity);
        };
    auto recordMacroBucket = [&](const OwnerStateFacts &facts) {
      for (const MacroStateIdentity &identity : facts.macroRequirements)
        recordMacroName(identity);
      for (const MacroStateObservation &observation :
           facts.macroExpansionObservations)
        recordMacroObservation(observation);
      for (const MacroStateObservation &observation :
           facts.definedOperatorObservations)
        recordMacroObservation(observation);
      for (const MacroStateObservation &observation :
           facts.conditionalMacroObservations)
        recordMacroObservation(observation);
    };
    auto recordCounterBucket = [&](const OwnerStateFacts &facts) {
      for (const CounterEventIdentity &identity : facts.counterEvents)
        appendKeyedIndex(graph.observerIndex.observersByCounterEvent,
                         counterKey(identity), siteIndex);
    };
    auto recordPragmaBucket = [&](const OwnerStateFacts &facts) {
      for (const PragmaStateIdentity &identity : facts.pragmaStateEvents)
        appendKeyedIndex(graph.observerIndex.observersByPragmaState,
                         pragmaKey(identity), siteIndex);
    };
    auto recordIncludeBucket = [&](const OwnerStateFacts &facts) {
      for (const IncludeStateIdentity &identity : facts.includeStateEvents)
        appendKeyedIndex(graph.observerIndex.observersByIncludeState,
                         includeKey(identity), siteIndex);
      for (const IncludeGuardStateIdentity &identity :
           facts.includeGuardStateEvents)
        appendKeyedIndex(graph.observerIndex.observersByIncludeGuard,
                         includeGuardKey(identity), siteIndex);
    };
    auto recordConditionalBucket = [&](const OwnerStateFacts &facts) {
      for (const ConditionalStateIdentity &identity :
           facts.conditionalStateEvents)
        appendKeyedIndex(graph.observerIndex.observersByConditionalState,
                         conditionalKey(identity), siteIndex);
    };

    switch (component) {
    case OwnerStateComponent::MacroState:
    case OwnerStateComponent::DefinedOperator:
      recordMacroBucket(delta.entry);
      recordMacroBucket(delta.observes);
      break;
    case OwnerStateComponent::ConditionalState:
      recordMacroBucket(delta.entry);
      recordMacroBucket(delta.observes);
      recordConditionalBucket(delta.entry);
      recordConditionalBucket(delta.observes);
      break;
    case OwnerStateComponent::Counter:
      recordCounterBucket(delta.entry);
      recordCounterBucket(delta.observes);
      break;
    case OwnerStateComponent::PragmaState:
      recordPragmaBucket(delta.entry);
      recordPragmaBucket(delta.observes);
      break;
    case OwnerStateComponent::IncludeGuardState:
    case OwnerStateComponent::IncludeState:
      recordIncludeBucket(delta.entry);
      recordIncludeBucket(delta.observes);
      break;
    case OwnerStateComponent::LineNumber:
      appendKeyedIndex(graph.observerIndex.observersByConditionalState,
                       "line-state", siteIndex);
      break;
    case OwnerStateComponent::FileState:
      appendKeyedIndex(graph.observerIndex.observersByConditionalState,
                       "file-state", siteIndex);
      break;
    case OwnerStateComponent::FileName:
      appendKeyedIndex(graph.observerIndex.observersByConditionalState,
                       "filename-state", siteIndex);
      break;
    case OwnerStateComponent::UnmodeledState:
    case OwnerStateComponent::Unknown:
      break;
    }
  };

  auto countMissingFacts = [](const OwnerStateDelta &delta) -> uint64_t {
    return delta.entry.missingStateFacts.size() +
           delta.observes.missingStateFacts.size() +
           delta.mutates.missingStateFacts.size() +
           delta.exit.missingStateFacts.size();
  };

  auto componentIsMutatedByDelta = [](const OwnerStateDelta &delta,
                                      OwnerStateComponent component) {
    auto bucketMutates = [&](const OwnerStateFacts &facts) {
      switch (component) {
      case OwnerStateComponent::MacroState:
        return facts.MutatesMacroState();
      case OwnerStateComponent::DefinedOperator:
        return false;
      case OwnerStateComponent::ConditionalState:
        return facts.MutatesConditionalState();
      case OwnerStateComponent::LineNumber:
      case OwnerStateComponent::FileState:
      case OwnerStateComponent::FileName:
        return facts.MutatesLineFileState();
      case OwnerStateComponent::Counter:
        return facts.HasCounterEvents();
      case OwnerStateComponent::PragmaState:
        return facts.MutatesPragmaState();
      case OwnerStateComponent::IncludeGuardState:
        return facts.MutatesIncludeGuardState();
      case OwnerStateComponent::IncludeState:
        return facts.MutatesIncludeState();
      case OwnerStateComponent::UnmodeledState:
        return facts.HasMissingFactKind(
            MissingStateFactKind::MissingOwnerOrderingFacts);
      case OwnerStateComponent::Unknown:
        return false;
      }
      llvm_unreachable("Invalid owner state component");
    };
    return bucketMutates(delta.mutates) || bucketMutates(delta.exit);
  };

  auto rebuildObserverIndex = [&]() {
    graph.observerSites.clear();
    graph.audit = OwnerStateGraphAuditStats();
    graph.audit.ownerNodes = graph.nodes.size();
    uint64_t observerOrder = 0;
    for (const OwnerStateGraphNode &node : graph.nodes) {
      if (node.IsZeroTokenEvent() &&
          node.kind != OwnerStateGraphNodeKind::OrdinaryTokenOwner)
        ++graph.audit.zeroTokenStateNodes;
      graph.audit.missingProducerFacts += countMissingFacts(node.state);

      const OwnerClosure &closure = node.closure;
      for (OwnerStateComponent component : components) {
        if (componentIsMutatedByDelta(node.state, component))
          ++graph.audit.mutatedStateComponents;
        if (!OwnerObserverSummaryObservesComponent(closure.observers,
                                                   component))
          continue;
        SuffixStateObserverSite site;
        site.nodeId = node.id;
        site.nodeKind = node.kind;
        site.owner = closure.owner;
        site.source = node.source;
        site.aTokens = node.aTokens;
        site.component = component;
        site.observationKind = ObservationKindForComponent(component);
        site.observations = closure.observers;
        site.order = observerOrder++;
        site.detail =
            llvm::formatv("{0} node observes {1}", node.kind, component).str();
        const uint64_t siteIndex = graph.observerSites.size();
        graph.observerSites.push_back(std::move(site));
        recordComponentIndex(component, siteIndex);
        recordKeyedIndexes(component, node.state, siteIndex);
        ++graph.audit.observedStateComponents;
      }

      if (OwnerStateDeltaHasUnmodeledState(closure.stateIn) ||
          OwnerStateDeltaHasUnmodeledState(closure.stateOut)) {
        SuffixStateObserverSite site;
        site.nodeId = node.id;
        site.nodeKind = node.kind;
        site.owner = closure.owner;
        site.source = node.source;
        site.aTokens = node.aTokens;
        site.component = OwnerStateComponent::UnmodeledState;
        site.observationKind = ObservationKindForComponent(site.component);
        site.observations = closure.observers;
        site.order = observerOrder++;
        site.detail =
            llvm::formatv("{0} node has unmodeled state", node.kind).str();
        const uint64_t siteIndex = graph.observerSites.size();
        graph.observerSites.push_back(std::move(site));
        recordComponentIndex(site.component, siteIndex);
        ++graph.audit.observedStateComponents;
      }

      if (!node.source.IsComplete() && !node.HasTokenAnchor())
        ++graph.audit.incomparableNodes;
    }
  };

  const size_t regularNodeCount = graph.nodes.size();
  for (size_t nodeIndex = 0; nodeIndex < regularNodeCount; ++nodeIndex) {
    const OwnerStateGraphNode &node = graph.nodes[nodeIndex];
    const OwnerClosure &closure = node.closure;
    // requires __COUNTER__ to be represented as a semantic state event
    // in its own right.  BuildOwnerStateDelta() already attaches precise
    // CounterEventIdentity values to the macro owner; this loop splits them
    // into explicit graph nodes without inventing new counter facts.
    const OwnerStateDelta delta = closure.stateOut;
    for (const CounterEventIdentity &counter : delta.mutates.counterEvents) {
      OwnerSourceRange source = node.source;
      if (!counter.expansionSiteFile.empty() && counter.expansionSiteBegin &&
          counter.expansionSiteEnd) {
        source = OwnerSourceRange::From(
            counter.expansionSiteFile, *counter.expansionSiteBegin,
            *counter.expansionSiteEnd, counter.ownerIncludeId);
      }
      OwnerStateGraphNode counterNode;
      counterNode.kind = OwnerStateGraphNodeKind::CounterEvent;
      counterNode.closure = closure;
      counterNode.state.mutates.AddCounterEvent(counter);
      counterNode.state.exit.AddCounterEvent(counter);
      counterNode.source = source;
      counterNode.aTokens =
          OwnerTokenRange::From(counter.aTokenBegin, counter.aTokenEnd);
      counterNode.containingIncludeId = counter.ownerIncludeId;
      counterNode.containingMacroInvocationId = closure.owner.macroInvocationId;
      counterNode.containingConditionalArmId = closure.owner.condArmId;
      counterNode.detail =
          llvm::formatv("counter event from owner {0}", closure.owner.kind)
              .str();
      if (auditDirectState)
        AuditDirectStateCheckClosure(
            DirectStateCheckKind::CounterEvent, OwnerStateComponent::Counter,
            DirectStateCheckClosureKind::OwnerStateGraphEdge,
            StateMutationKind::Unknown, "owner-state-graph",
            counterNode.detail);
      graph.nodes.push_back(std::move(counterNode));
    }
  }

  assignGraphOrder();
  rebuildObserverIndex();

  return graph;
}

const OwnerStateGraph &RefoldOwnerStateProof::GetOwnerStateGraph() const {
  if (!ownerStateGraphCache_) {
    ownerStateGraphCache_ = BuildOwnerStateGraph();
    const OwnerStateGraphAuditStats &audit = ownerStateGraphCache_->audit;
    theoremAudit_.RecordOwnerStateGraphAudit(audit);
    REFOLD_LOG_TRACE("state/graph",
                     "owner-state graph: nodes={0} zeroTokenStateNodes={1} "
                     "observedComponents={2} mutatedComponents={3} "
                     "incomparableNodes={4} missingProducerFacts={5}",
                     audit.ownerNodes, audit.zeroTokenStateNodes,
                     audit.observedStateComponents,
                     audit.mutatedStateComponents, audit.incomparableNodes,
                     audit.missingProducerFacts);
  }
  return *ownerStateGraphCache_;
}

SuffixObserverQueryResult RefoldOwnerStateProof::FindSuffixObservers(
    const OwnerStateBoundary &boundary, OwnerStateComponent component) const {
  SuffixObserverQueryResult result;
  if (component == OwnerStateComponent::Unknown)
    return result;

  const OwnerStateGraph &graph = GetOwnerStateGraph();
  const ArrayRef<uint64_t> siteIndexes =
      ObserverSiteIndexesForComponent(graph, component);

  auto sourceComparable = [&](const OwnerSourceRange &site) -> bool {
    return boundary.hasSourceBoundary && boundary.source.HasPath() &&
           site.HasPath() &&
           paths_.PathsEqual(boundary.source.path, site.path) &&
           boundary.source.includeId == site.includeId;
  };

  auto sourceIsAfterBoundary = [&](const OwnerSourceRange &site) -> bool {
    // A site that begins after the boundary is a suffix observer.  A site that
    // straddles the boundary is also relevant: preserving the suffix of that
    // same owner can still observe state repaired at the boundary.
    return site.begin >= boundary.source.end ||
           (site.begin < boundary.source.end && site.end > boundary.source.end);
  };

  auto tokenComparable = [&](const OwnerTokenRange &tokens) -> bool {
    return boundary.hasTokenBoundary && boundary.aTokens.IsValid() &&
           !boundary.aTokens.Empty() && tokens.IsValid() && !tokens.Empty();
  };

  auto tokenIsAfterBoundary = [&](const OwnerTokenRange &tokens) -> bool {
    return tokens.begin >= boundary.aTokens.end ||
           (tokens.begin < boundary.aTokens.end &&
            tokens.end > boundary.aTokens.end);
  };

  auto makeObserverResult = [&](uint64_t siteIndex,
                                const SuffixStateObserverSite &site,
                                SuffixOrderingProofKind orderingProof) {
    SuffixObserverResult observer;
    observer.observerSiteIndex = siteIndex;
    observer.nodeId = site.nodeId;
    observer.firstObserver = site.owner;
    observer.component = site.component;
    observer.observationKind = site.observationKind;
    observer.orderingProof = orderingProof;
    observer.site = site;
    // Exposes repair/materialization affordances but does not invent a repair.
    // A source-ordered observer gives the gateway a concrete place before the
    // observer where repair could be inserted.  Token-only ordering can still
    // prove suffix order, but not a source insertion point.
    observer.repairCanPrecede =
        orderingProof == SuffixOrderingProofKind::SourceOrder ||
        orderingProof == SuffixOrderingProofKind::SourceAndTokenOrder;
    observer.materializable =
        site.source.IsComplete() || site.aTokens.IsValid();
    return observer;
  };

  for (uint64_t siteIndex : siteIndexes) {
    if (siteIndex >= graph.observerSites.size())
      continue;
    const SuffixStateObserverSite &site = graph.observerSites[siteIndex];
    if (site.component != component)
      continue;

    bool comparable = false;
    bool sourceAfter = false;
    bool tokenAfter = false;

    if (sourceComparable(site.source)) {
      comparable = true;
      sourceAfter = sourceIsAfterBoundary(site.source);
    }

    if (tokenComparable(site.aTokens)) {
      comparable = true;
      tokenAfter = tokenIsAfterBoundary(site.aTokens);
    }

    if (comparable) {
      if (sourceAfter || tokenAfter) {
        SuffixOrderingProofKind proof = SuffixOrderingProofKind::Unknown;
        if (sourceAfter && tokenAfter)
          proof = SuffixOrderingProofKind::SourceAndTokenOrder;
        else if (sourceAfter)
          proof = SuffixOrderingProofKind::SourceOrder;
        else
          proof = SuffixOrderingProofKind::TokenOrder;

        result.sites.push_back(site);
        result.orderedObservers.push_back(
            makeObserverResult(siteIndex, site, proof));
        if (!result.firstObserver)
          result.firstObserver = result.orderedObservers.back();
      }
      continue;
    }

    // The indexed graph found a real observer for the requested component, but
    // the boundary and observer could not be ordered with available
    // source/token facts.  Preserve this as a named no-canonical-order
    // condition so the proof model cannot accidentally treat missing producer
    // order as suffix stability.
    result.hasIncomparableObserver = true;
    result.hasNoCanonicalSuffixOrder = true;
    result.incomparableObservers.MergeFrom(site.observations);
    result.incomparableResults.push_back(makeObserverResult(
        siteIndex, site,
        SuffixOrderingProofKind::IncomparableMissingProducerFacts));
    theoremAudit_.RecordGraphIncomparableNode();
  }

  return result;
}

OwnerStateComponent RefoldOwnerStateProof::StateComponentForMissingStateFact(
    MissingStateFactKind kind) {
  switch (kind) {
  case MissingStateFactKind::MissingMacroFacts:
    return OwnerStateComponent::MacroState;
  case MissingStateFactKind::MissingLineControlFacts:
    return OwnerStateComponent::LineNumber;
  case MissingStateFactKind::MissingCounterFacts:
    return OwnerStateComponent::Counter;
  case MissingStateFactKind::MissingPragmaFacts:
    return OwnerStateComponent::PragmaState;
  case MissingStateFactKind::MissingIncludeGuardFacts:
    return OwnerStateComponent::IncludeGuardState;
  case MissingStateFactKind::MissingConditionalFacts:
    return OwnerStateComponent::ConditionalState;
  case MissingStateFactKind::MissingOwnerOrderingFacts:
    return OwnerStateComponent::UnmodeledState;
  }
  llvm_unreachable("Invalid missing state fact kind");
}

TerminalFallbackProofFailure
RefoldOwnerStateProof::MissingStateFactTerminalFailure(
    MissingStateFactKind kind, StringRef detail) {
  TerminalFallbackProofFailure failure =
      SuffixStabilityTerminalFailureForComponent(
          StateComponentForMissingStateFact(kind));
  failure.context.stateComponent = formatv("{0}:{1}", kind, detail).str();
  if (kind == MissingStateFactKind::MissingOwnerOrderingFacts) {
    failure.obligation = TerminalFallbackObligationKind::ProducerFactsAvailable;
    failure.reason = TerminalFallbackFailureReason::MissingProducerFacts;
    failure.theoremFailure =
        NormalizeTerminalFallbackFailureReason(
            TerminalFallbackFailureReason::MissingProducerFacts)
            .value_or(TheoremFallbackFailureKind::Unknown);
  }
  return failure;
}

std::vector<OwnerStateComponent>
RefoldOwnerStateProof::StateComponentsMutatedByDelta(
    const OwnerStateDelta &summary) {
  const OwnerStateDelta theoremDelta = summary;
  const StateMutations &mutations = theoremDelta.mutates;
  std::vector<OwnerStateComponent> components;
  auto appendUnique = [&](OwnerStateComponent component) {
    if (component == OwnerStateComponent::Unknown)
      return;
    if (llvm::none_of(components, [&](OwnerStateComponent existing) {
          return existing == component;
        }))
      components.push_back(component);
  };

  if (mutations.MutatesMacroState())
    appendUnique(OwnerStateComponent::MacroState);
  if (mutations.MutatesConditionalState())
    appendUnique(OwnerStateComponent::ConditionalState);
  if (mutations.MutatesLineFileState()) {
    appendUnique(OwnerStateComponent::LineNumber);
    appendUnique(OwnerStateComponent::FileState);
    appendUnique(OwnerStateComponent::FileName);
  }
  if (mutations.HasCounterEvents())
    appendUnique(OwnerStateComponent::Counter);
  if (mutations.MutatesPragmaState())
    appendUnique(OwnerStateComponent::PragmaState);
  if (mutations.MutatesIncludeGuardState())
    appendUnique(OwnerStateComponent::IncludeGuardState);
  if (mutations.MutatesIncludeState())
    appendUnique(OwnerStateComponent::IncludeState);

  // component-specific missing facts are state obligations, not
  // absence of state.  Project them to the component they make uncertain so a
  // caller cannot accidentally treat a missing producer fact as a safe empty
  // summary.
  for (const MissingStateFact &fact : mutations.missingStateFacts) {
    if (fact.kind == MissingStateFactKind::MissingLineControlFacts) {
      appendUnique(OwnerStateComponent::LineNumber);
      appendUnique(OwnerStateComponent::FileState);
      appendUnique(OwnerStateComponent::FileName);
      continue;
    }
    appendUnique(StateComponentForMissingStateFact(fact.kind));
  }

  if (mutations.HasMissingFactKind(
          MissingStateFactKind::MissingOwnerOrderingFacts))
    appendUnique(OwnerStateComponent::UnmodeledState);
  return components;
}

TerminalFallbackProofFailure
RefoldOwnerStateProof::SuffixStabilityTerminalFailureForComponent(
    OwnerStateComponent component) {
  switch (component) {
  case OwnerStateComponent::MacroState:
  case OwnerStateComponent::DefinedOperator:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::MacroStateStabilizable,
        TerminalFallbackFailureReason::MacroStateNotStabilizable,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  case OwnerStateComponent::ConditionalState:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::ConditionalStateStabilizable,
        TerminalFallbackFailureReason::ConditionalStateNotStabilizable,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  case OwnerStateComponent::LineNumber:
  case OwnerStateComponent::FileState:
  case OwnerStateComponent::FileName:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::LineControlStateProducerProven,
        TerminalFallbackFailureReason::LineControlStateNotProducerProven,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  case OwnerStateComponent::Counter:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::CounterStateStabilizable,
        TerminalFallbackFailureReason::CounterStateNotStabilizable,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  case OwnerStateComponent::PragmaState:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::PragmaBoundaryKnown,
        TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  case OwnerStateComponent::IncludeGuardState:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::IncludeGuardStateStabilizable,
        TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  case OwnerStateComponent::IncludeState:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::StateTransitionClosure,
        TerminalFallbackFailureReason::StateTransitionConsumedAndObserved,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  case OwnerStateComponent::UnmodeledState:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::ProducerFactsAvailable,
        TerminalFallbackFailureReason::MissingProducerFacts,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  case OwnerStateComponent::Unknown:
    return MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::StateTransitionClosure,
        TerminalFallbackFailureReason::StateTransitionConsumedAndObserved,
        TerminalFallbackFailureContext::ForStateComponent(toString(component)));
  }
  llvm_unreachable("Invalid owner state component");
}

TerminalFallbackProofFailure
RefoldOwnerStateProof::ReverseSolvedDirectiveTerminalFailure(
    OwnerStateComponent component, const OwnerStateBoundary &boundary,
    StringRef directiveKind, StringRef detail) {
  TerminalFallbackFailureContext context =
      TerminalFallbackFailureContext::ForStateComponent(toString(component));
  if (boundary.hasSourceBoundary && boundary.source.IsComplete()) {
    context.sourcePath = boundary.source.path;
    context.sourceBegin = boundary.source.begin;
    context.sourceEnd = boundary.source.end;
  }
  if (boundary.hasTokenBoundary && boundary.aTokens.IsValid()) {
    context.aTokenBegin = boundary.aTokens.begin;
    context.aTokenEnd = boundary.aTokens.end;
  }
  (void)directiveKind;
  (void)detail;
  return MakeTerminalFallbackProofFailure(
      TerminalFallbackObligationKind::ReverseSolvedDirectiveForbidden,
      TerminalFallbackFailureReason::ReverseSolvedDirectiveRequired,
      std::move(context));
}

SuffixStabilityWitness
RefoldOwnerStateProof::BuildReverseSolvedDirectiveTerminalWitness(
    const OwnerStateBoundary &boundary, OwnerStateComponent component,
    StringRef directiveKind, StringRef detail) {
  TerminalStateFailureWitness witness;
  witness.component = component;
  witness.boundary = boundary;
  witness.failure = ReverseSolvedDirectiveTerminalFailure(
      component, boundary, directiveKind, detail);
  witness.hasFailure = true;
  witness.detail = detail.str();
  return SuffixStabilityWitness::From(std::move(witness));
}

OwnerStateComponent
RefoldOwnerStateProof::ComponentNamedBySuffixStabilityWitness(
    const SuffixStabilityWitness &witness) {
  switch (witness.kind) {
  case SuffixStabilityWitnessKind::SuffixUnobserved:
    return witness.suffixUnobserved ? witness.suffixUnobserved->component
                                    : OwnerStateComponent::Unknown;
  case SuffixStabilityWitnessKind::StateRepair:
    return witness.stateRepair ? witness.stateRepair->component
                               : OwnerStateComponent::Unknown;
  case SuffixStabilityWitnessKind::OwnerMaterialization:
    return witness.ownerMaterialization
               ? witness.ownerMaterialization->component
               : OwnerStateComponent::Unknown;
  case SuffixStabilityWitnessKind::ClosureWidening:
    return witness.closureWidening ? witness.closureWidening->component
                                   : OwnerStateComponent::Unknown;
  case SuffixStabilityWitnessKind::Literalization:
    return witness.literalization ? witness.literalization->component
                                  : OwnerStateComponent::Unknown;
  case SuffixStabilityWitnessKind::TerminalStateFailure:
    return witness.terminalFailure ? witness.terminalFailure->component
                                   : OwnerStateComponent::Unknown;
  case SuffixStabilityWitnessKind::None:
    return OwnerStateComponent::Unknown;
  }
  llvm_unreachable("Invalid suffix-stability witness kind");
}

bool RefoldOwnerStateProof::SuffixStabilityWitnessNamesComponent(
    const SuffixStabilityWitness &witness, OwnerStateComponent component) {
  if (component == OwnerStateComponent::Unknown)
    return false;
  return ComponentNamedBySuffixStabilityWitness(witness) == component;
}

StateTransitionProof
RefoldOwnerStateProof::CheckStateTransitionAcrossEditBoundary(
    const StateTransitionGatewayRequest &request) const {
  theoremAudit_.RecordStateTransitionGatewayCheck();
  const bool auditDirectState = theoremAudit_.IsNoLegacyAuditEnabled();
  if (auditDirectState)
    AuditDirectStateCheckClosure(
        DirectStateCheckKindForComponent(request.component), request.component,
        DirectStateCheckClosureKind::StateTransitionGatewayWitness,
        request.mutation, request.stage, request.detail);

  StateTransitionProof proof;
  proof.before = request.before;
  proof.after = request.after;
  SuffixStabilityWitness acceptedWitness = request.witness;
  bool hasIncomparableObserver = false;
  size_t observerCount = 0;

  auto finalizeGatewayResult = [&]() -> StateTransitionProof {
    // The typed proof is the gateway result: it records the accepted suffix
    // witness, separates the
    // closure-widening subset for theorem consumers, and records a terminal
    // failure only when the gateway actually failed closed.
    if (acceptedWitness.kind != SuffixStabilityWitnessKind::None) {
      if (proof.suffixWitnesses.empty())
        proof.suffixWitnesses.push_back(acceptedWitness);
      if (acceptedWitness.kind == SuffixStabilityWitnessKind::ClosureWidening &&
          acceptedWitness.closureWidening && proof.wideningWitnesses.empty())
        proof.wideningWitnesses.push_back(*acceptedWitness.closureWidening);
    }
    return proof;
  };

  auto noteGatewayViolation = [&](StringRef detail) {
    theoremAudit_.RecordStateTransitionGatewayViolation(
        llvm::formatv("state-transition gateway audit violation: {0}; "
                      "component={1} mutation={2} witness={3} stage={4} "
                      "detail={5}",
                      detail, request.component, request.mutation,
                      request.witness.kind, request.stage, request.detail)
            .str());
  };

  auto requestTerminal = [&](TerminalFallbackProofFailure failure,
                             std::string message) {
    proof.failure = failure;
    theoremAudit_.RecordStateTransitionGatewayTerminalFailure();
    if (!theoremAudit_.AuditTerminalFallbackProofFailure(failure,
                                                         request.stage))
      noteGatewayViolation("terminal state failure lacked a classified "
                           "component-specific obligation");
    if (auditDirectState)
      AuditDirectStateCheckClosure(
          DirectStateCheckKindForComponent(request.component),
          request.component,
          DirectStateCheckClosureKind::TerminalFallbackProofFailure,
          request.mutation, request.stage, message);
    terminalSink_.RequestTerminalFallback(std::move(failure), request.stage,
                                          message);
    return finalizeGatewayResult();
  };

  if (request.component == OwnerStateComponent::Unknown) {
    theoremAudit_.RecordStateTransitionUnknownComponentViolation();
    noteGatewayViolation("request named Unknown state component");
    return requestTerminal(
        SuffixStabilityTerminalFailureForComponent(request.component),
        llvm::formatv("state-transition gateway received unknown component: "
                      "mutation={0} witness={1} detail={2}",
                      request.mutation, request.witness.kind, request.detail)
            .str());
  }

  if (request.mutation == StateMutationKind::Unknown) {
    theoremAudit_.RecordStateTransitionUnknownMutationViolation();
    noteGatewayViolation("request named Unknown state mutation");
    return requestTerminal(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::ProducerFactsAvailable,
            TerminalFallbackFailureReason::MissingProducerFacts,
            TerminalFallbackFailureContext::ForStateComponent(
                toString(request.component))),
        llvm::formatv("state-transition gateway received unknown mutation: "
                      "component={0} witness={1} detail={2}",
                      request.component, request.witness.kind, request.detail)
            .str());
  }

  if (request.requiresDirectiveClosureProof) {
    const StringRef directiveLabel = request.directiveKind.empty()
                                         ? StringRef("<unknown>")
                                         : StringRef(request.directiveKind);
    if (request.directiveClosureStatus !=
        DirectiveClosureStatus::DirectiveOwnerInsideAcceptedClosure) {
      return requestTerminal(
          ReverseSolvedDirectiveTerminalFailure(request.component,
                                                request.boundary,
                                                directiveLabel, request.detail),
          llvm::formatv("state-transition gateway rejected reverse-solved "
                        "directive rewrite: component={0} mutation={1} "
                        "directive={2} closureStatus={3} stage={4} detail={5}",
                        request.component, request.mutation, directiveLabel,
                        request.directiveClosureStatus, request.stage,
                        request.detail)
              .str());
    }

    REFOLD_LOG_TRACE(
        "state/gateway",
        "state-transition gateway accepted directive-closure proof before "
        "suffix query: component={0} mutation={1} directive={2} stage={3} "
        "detail={4}",
        request.component, request.mutation, directiveLabel, request.stage,
        request.detail);
  }

  const SuffixObserverQueryResult observers =
      FindSuffixObservers(request.boundary, request.component);
  observerCount = observers.sites.size();
  hasIncomparableObserver = observers.hasIncomparableObserver;
  const bool hasObserver = !observers.sites.empty() || hasIncomparableObserver;

  if (!hasObserver) {
    if (!request.requireKnownObserver) {
      theoremAudit_.RecordStateTransitionGatewayStable();
      SuffixUnobservedWitness witness;
      witness.component = request.component;
      witness.boundary = request.boundary;
      witness.detail = request.detail;
      acceptedWitness = SuffixStabilityWitness::From(std::move(witness));
      REFOLD_LOG_TRACE(
          "state/gateway",
          "state-transition gateway discharged: component={0} "
          "mutation={1} no preserved suffix observer stage={2} detail={3}",
          request.component, request.mutation, request.stage, request.detail);
      return finalizeGatewayResult();
    }

    return requestTerminal(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::ProducerFactsAvailable,
            TerminalFallbackFailureReason::NoCanonicalSuffixOrder,
            TerminalFallbackFailureContext::ForStateComponent(
                toString(request.component))),
        llvm::formatv(
            "state-transition gateway could not model or canonically order the "
            "required observer: component={0} mutation={1} witness={2} "
            "detail={3}",
            request.component, request.mutation, request.witness.kind,
            request.detail)
            .str());
  }

  SuffixStabilityWitness normalizedWitness = request.witness;
  auto attachFirstObserverToWitness = [&]() {
    if (!observers.firstObserver)
      return;
    switch (normalizedWitness.kind) {
    case SuffixStabilityWitnessKind::StateRepair:
      if (normalizedWitness.stateRepair &&
          !normalizedWitness.stateRepair->firstObserver)
        normalizedWitness.stateRepair->firstObserver = observers.firstObserver;
      break;
    case SuffixStabilityWitnessKind::OwnerMaterialization:
      if (normalizedWitness.ownerMaterialization &&
          !normalizedWitness.ownerMaterialization->materializedObserver)
        normalizedWitness.ownerMaterialization->materializedObserver =
            observers.firstObserver;
      break;
    case SuffixStabilityWitnessKind::ClosureWidening:
      if (normalizedWitness.closureWidening &&
          !normalizedWitness.closureWidening->widenedThroughObserver)
        normalizedWitness.closureWidening->widenedThroughObserver =
            observers.firstObserver;
      break;
    case SuffixStabilityWitnessKind::Literalization:
      if (normalizedWitness.literalization &&
          !normalizedWitness.literalization->literalizedObserver)
        normalizedWitness.literalization->literalizedObserver =
            observers.firstObserver;
      break;
    case SuffixStabilityWitnessKind::None:
    case SuffixStabilityWitnessKind::SuffixUnobserved:
    case SuffixStabilityWitnessKind::TerminalStateFailure:
      break;
    }
  };

  switch (normalizedWitness.kind) {
  case SuffixStabilityWitnessKind::StateRepair:
  case SuffixStabilityWitnessKind::OwnerMaterialization:
  case SuffixStabilityWitnessKind::ClosureWidening:
  case SuffixStabilityWitnessKind::Literalization:
    if (!SuffixStabilityWitnessNamesComponent(normalizedWitness,
                                              request.component)) {
      theoremAudit_.RecordStateTransitionNoneWitnessViolation();
      noteGatewayViolation("typed state witness did not name the requested "
                           "state component");
      return requestTerminal(
          SuffixStabilityTerminalFailureForComponent(request.component),
          llvm::formatv(
              "state-transition gateway rejected mismatched typed "
              "witness: component={0} mutation={1} witness={2} "
              "witnessComponent={3} detail={4}",
              request.component, request.mutation, normalizedWitness.kind,
              ComponentNamedBySuffixStabilityWitness(normalizedWitness),
              request.detail)
              .str());
    }
    attachFirstObserverToWitness();
    theoremAudit_.RecordStateTransitionGatewayStable();
    acceptedWitness = std::move(normalizedWitness);
    REFOLD_LOG_TRACE(
        "state/gateway",
        "state-transition gateway discharged: component={0} mutation={1} "
        "witness={2} orderedObservers={3} incomparable={4} stage={5} "
        "detail={6}",
        request.component, request.mutation, acceptedWitness.kind,
        static_cast<uint64_t>(observerCount),
        hasIncomparableObserver ? "YES" : "NO", request.stage, request.detail);
    return finalizeGatewayResult();

  case SuffixStabilityWitnessKind::TerminalStateFailure: {
    TerminalFallbackProofFailure failure =
        SuffixStabilityTerminalFailureForComponent(request.component);
    if (!SuffixStabilityWitnessNamesComponent(normalizedWitness,
                                              request.component)) {
      theoremAudit_.RecordStateTransitionNoneWitnessViolation();
      noteGatewayViolation("terminal state witness did not name the requested "
                           "state component");
    }
    if (normalizedWitness.terminalFailure &&
        normalizedWitness.terminalFailure->hasFailure)
      failure = normalizedWitness.terminalFailure->failure;
    return requestTerminal(
        std::move(failure),
        llvm::formatv("state-transition gateway terminal witness: "
                      "component={0} mutation={1} orderedObservers={2} "
                      "incomparable={3} detail={4}",
                      request.component, request.mutation,
                      static_cast<uint64_t>(observerCount),
                      hasIncomparableObserver ? "YES" : "NO", request.detail)
            .str());
  }

  case SuffixStabilityWitnessKind::None:
  case SuffixStabilityWitnessKind::SuffixUnobserved:
    break;
  }

  theoremAudit_.RecordStateTransitionNoneWitnessViolation();
  noteGatewayViolation("suffix-observed state transition lacked a typed "
                       "repair/materialization/widening/literalization "
                       "witness");

  TerminalFallbackProofFailure failure =
      SuffixStabilityTerminalFailureForComponent(request.component);
  if (hasIncomparableObserver) {
    failure = MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::ProducerFactsAvailable,
        TerminalFallbackFailureReason::NoCanonicalSuffixOrder,
        TerminalFallbackFailureContext::ForStateComponent(
            toString(request.component)));
  }
  return requestTerminal(
      std::move(failure),
      llvm::formatv("state transition for component={0} mutation={1} has "
                    "preserved suffix observers but no typed "
                    "repair/materialization/widening/literalization witness: "
                    "orderedObservers={2} incomparable={3}; {4}",
                    request.component, request.mutation,
                    static_cast<uint64_t>(observerCount),
                    hasIncomparableObserver ? "YES" : "NO", request.detail)
          .str());
}

StateTransitionProof
RefoldOwnerStateProof::CheckReverseSolvedDirectiveAcrossEditBoundary(
    const OwnerStateBoundary &boundary, OwnerStateComponent component,
    StateMutationKind mutation, DirectiveClosureStatus directiveClosureStatus,
    StringRef directiveKind, StringRef stage, StringRef detail) const {
  StateTransitionGatewayRequest request;
  request.boundary = boundary;
  request.component = component;
  request.mutation = mutation;
  request.witness = SuffixStabilityWitness::None();
  request.stage = stage.str();
  request.detail = detail.str();
  request.requireKnownObserver = false;
  request.requiresDirectiveClosureProof = true;
  request.directiveClosureStatus = directiveClosureStatus;
  request.directiveKind = directiveKind.str();
  return CheckStateTransitionAcrossEditBoundary(request);
}

StateTransitionProof
RefoldOwnerStateProof::CheckStateTransitionAcrossEditBoundary(
    const OwnerStateBoundary &boundary, OwnerStateComponent component,
    StateMutationKind mutation, SuffixStabilityWitness witness, StringRef stage,
    StringRef detail, bool requireKnownObserver) const {
  StateTransitionGatewayRequest request;
  request.boundary = boundary;
  request.component = component;
  request.mutation = mutation;
  request.witness = std::move(witness);
  request.stage = stage.str();
  request.detail = detail.str();
  request.requireKnownObserver = requireKnownObserver;
  return CheckStateTransitionAcrossEditBoundary(request);
}

SuffixStabilityWitness RefoldOwnerStateProof::BuildStateTransitionWitness(
    SuffixStabilityWitnessKind kind, OwnerStateComponent component,
    const OwnerStateBoundary &boundary, StringRef detail) {
  switch (kind) {
  case SuffixStabilityWitnessKind::StateRepair: {
    StateRepairWitness witness;
    witness.component = component;
    witness.boundary = boundary;
    witness.detail = detail.str();
    return SuffixStabilityWitness::From(std::move(witness));
  }
  case SuffixStabilityWitnessKind::OwnerMaterialization: {
    OwnerMaterializationWitness witness;
    witness.component = component;
    witness.boundary = boundary;
    witness.detail = detail.str();
    return SuffixStabilityWitness::From(std::move(witness));
  }
  case SuffixStabilityWitnessKind::ClosureWidening: {
    ClosureWideningWitness witness;
    witness.component = component;
    witness.boundary = boundary;
    witness.detail = detail.str();
    return SuffixStabilityWitness::From(std::move(witness));
  }
  case SuffixStabilityWitnessKind::Literalization: {
    LiteralizationWitness witness;
    witness.component = component;
    witness.boundary = boundary;
    witness.detail = detail.str();
    return SuffixStabilityWitness::From(std::move(witness));
  }
  case SuffixStabilityWitnessKind::TerminalStateFailure: {
    TerminalStateFailureWitness witness;
    witness.component = component;
    witness.boundary = boundary;
    witness.failure = SuffixStabilityTerminalFailureForComponent(component);
    witness.hasFailure = true;
    witness.detail = detail.str();
    return SuffixStabilityWitness::From(std::move(witness));
  }
  case SuffixStabilityWitnessKind::None:
  case SuffixStabilityWitnessKind::SuffixUnobserved:
    return SuffixStabilityWitness::None();
  }
  llvm_unreachable("invalid state-transition witness kind");
}

OwnerStateBoundary RefoldOwnerStateProof::CounterStateBoundaryForEvent(
    const CounterEventIdentity &event) {
  OwnerTokenRange tokens =
      OwnerTokenRange::From(event.aTokenBegin, event.aTokenEnd);

  if (!event.expansionSiteFile.empty() && event.expansionSiteBegin &&
      event.expansionSiteEnd) {
    return OwnerStateBoundary::FromSourceAndATokens(
        OwnerSourceRange::From(event.expansionSiteFile,
                               *event.expansionSiteBegin,
                               *event.expansionSiteEnd, event.ownerIncludeId),
        tokens);
  }

  return OwnerStateBoundary::FromATokens(tokens);
}

std::string RefoldOwnerStateProof::FormatCounterEventForWitness(
    const CounterEventIdentity &event) {
  return llvm::formatv("counter_event:macro={0}:ordinal={1}:owner_include={2}:"
                       "caller={3}:A=[{4},{5}):AValue={6}:expectedB={7}:"
                       "site_file={8}:site=[{9},{10}):literalizable={11}:"
                       "materializable={12}",
                       event.macroInvocationId, event.occurrenceOrdinal,
                       event.ownerIncludeId.has_value()
                           ? llvm::formatv("{0}", *event.ownerIncludeId).str()
                           : std::string("none"),
                       event.callerMacroId.has_value()
                           ? llvm::formatv("{0}", *event.callerMacroId).str()
                           : std::string("none"),
                       event.aTokenBegin, event.aTokenEnd,
                       RefoldWitnessTrace::FormatWitnessTraceHash(event.aValue),
                       event.expectedBValue
                           ? RefoldWitnessTrace::FormatWitnessTraceHash(
                                 *event.expectedBValue)
                           : std::string("none"),
                       event.expansionSiteFile.empty()
                           ? std::string("none")
                           : RefoldWitnessTrace::FormatWitnessTraceHash(
                                 event.expansionSiteFile),
                       event.expansionSiteBegin.value_or(0),
                       event.expansionSiteEnd.value_or(0),
                       event.canStabilizeByLiteralization ? 1 : 0,
                       event.canStabilizeByMaterialization ? 1 : 0)
      .str();
}

bool RefoldOwnerStateProof::IsLineControlStateComponent(
    OwnerStateComponent component) {
  switch (component) {
  case OwnerStateComponent::LineNumber:
  case OwnerStateComponent::FileState:
  case OwnerStateComponent::FileName:
    return true;
  case OwnerStateComponent::MacroState:
  case OwnerStateComponent::DefinedOperator:
  case OwnerStateComponent::ConditionalState:
  case OwnerStateComponent::Counter:
  case OwnerStateComponent::PragmaState:
  case OwnerStateComponent::IncludeGuardState:
  case OwnerStateComponent::IncludeState:
  case OwnerStateComponent::UnmodeledState:
  case OwnerStateComponent::Unknown:
    return false;
  }
  llvm_unreachable("Invalid owner state component");
}

std::string RefoldOwnerStateProof::FormatLineControlEventForWitness(
    const LineControlStateIdentity &event) {
  return llvm::formatv("line_control_event:id={0}:file={1}:site={2}:[{3},{4}):"
                       "active={5}:producer={6}:line_after={7}:file_after={8}:"
                       "owner_include={9}:operands={10}",
                       event.eventId, event.physicalFile,
                       event.siteBegin.has_value() ? 1 : 0,
                       event.siteBegin.value_or(0), event.siteEnd.value_or(0),
                       event.active ? 1 : 0, event.producerProven ? 1 : 0,
                       event.logicalLineAfter, event.logicalFileAfter,
                       event.ownerIncludeId.has_value()
                           ? llvm::formatv("{0}", *event.ownerIncludeId).str()
                           : std::string("none"),
                       event.operandProvenance)
      .str();
}

std::string RefoldOwnerStateProof::FormatBuiltinLocationObservationForWitness(
    const BuiltinLocationObservation &observation) {
  return llvm::formatv(
             "builtin_location:{0}:owner_include={1}:source={2}:[{3},{4}):"
             "a_tokens={5}:[{6},{7})",
             observation.kind,
             observation.ownerIncludeId.has_value()
                 ? llvm::formatv("{0}", *observation.ownerIncludeId).str()
                 : std::string("none"),
             observation.sourceBegin.has_value() &&
                     observation.sourceEnd.has_value()
                 ? 1
                 : 0,
             observation.sourceBegin.value_or(0),
             observation.sourceEnd.value_or(0),
             observation.aTokenBegin.has_value() &&
                     observation.aTokenEnd.has_value()
                 ? 1
                 : 0,
             observation.aTokenBegin.value_or(0),
             observation.aTokenEnd.value_or(0))
      .str();
}

OwnerStateBoundary RefoldOwnerStateProof::IncludeStateBoundaryForIncludeSite(
    const RefoldModel::IncludeItem &include) {
  return OwnerStateBoundary::FromSourceAndATokens(
      OwnerSourceRange::From(include.sitePath, include.siteB, include.siteE,
                             include.parent),
      OwnerTokenRange::From(include.cover.begin, include.cover.end));
}

} // namespace refold
} // namespace clang

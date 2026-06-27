//===--- RefoldOwnerStateTypes.h -------------------------------*- C++ -*-===//
//
// Owner-state proof carrier vocabulary for clang-refold.
//
// These records are deliberately value-only.  They describe owner identity,
// producer-state facts, state deltas, suffix observers, and state-transition
// witnesses used by RefoldOwnerStateProof and by higher-level proof lattice
// carriers.  They must not acquire RefoldEngine orchestration behavior or
// mutable engine caches; those belong in RefoldOwnerStateProof itself.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATETYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATETYPES_H

#include "core/RefoldModel.h"
#include "proof/RefoldTerminalProof.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

using llvm::ArrayRef;
using llvm::StringRef;

// ---------------------------- Ownership Helpers ----------------------------

/// Logical owner class used by the theorem-facing closure model.
///
/// Existing emission paths still materialize TU and include owners directly,
/// while macro and conditional owners are often represented by specialized
/// proof carriers.  The closed-domain model treats all of them as instances
/// of the same owner identity so future tiling, state-summary composition,
/// and terminal out-of-domain diagnostics can reason through one interface
/// instead of through owner-specific fallback ladders.
enum class OwnerKind {
  TU,
  Include,
  MacroInvocation,
  MacroDirective,
  LineControlIsland,
  PragmaIsland,
  ConditionalGroup,
  ConditionalArm,
  Unknown
};

inline StringRef toString(OwnerKind kind) {
  switch (kind) {
  case OwnerKind::TU:
    return "TU";
  case OwnerKind::Include:
    return "Include";
  case OwnerKind::MacroInvocation:
    return "MacroInvocation";
  case OwnerKind::MacroDirective:
    return "MacroDirective";
  case OwnerKind::LineControlIsland:
    return "LineControlIsland";
  case OwnerKind::PragmaIsland:
    return "PragmaIsland";
  case OwnerKind::ConditionalGroup:
    return "ConditionalGroup";
  case OwnerKind::ConditionalArm:
    return "ConditionalArm";
  case OwnerKind::Unknown:
    return "Unknown";
  }
  llvm_unreachable("Invalid owner kind");
}


/// Stable identity for one refolding owner.
///
/// `Owner` is deliberately small and value-semantic: it names the owner only.
/// Source intervals, A/B token envelopes, state summaries, and suffix
/// observers live in `OwnerClosure` below.  Keeping identity separate from
/// closure facts prevents call sites from accidentally treating a classified
/// owner as a discharged proof.
struct Owner {
  OwnerKind kind = OwnerKind::Unknown;
  // non-nullopt only when kind == Include.
  std::optional<uint64_t> includeId;
  // non-nullopt only when kind == MacroInvocation.
  std::optional<uint64_t> macroInvocationId;
  // non-nullopt only when kind == MacroDirective.
  std::optional<uint64_t> macroDirectiveId;
  // non-nullopt only when kind == LineControlIsland.
  std::optional<uint64_t> lineControlId;
  // non-nullopt only when kind == PragmaIsland.
  std::optional<uint64_t> pragmaId;
  // non-nullopt only when kind == ConditionalGroup.
  std::optional<uint64_t> condGroupId;
  // nullable; non-nullopt when the owner is in or is a specific conditional
  // arm.
  std::optional<uint64_t> condArmId;

  static Owner TU(std::optional<uint64_t> condArmId = std::nullopt) {
    Owner o;
    o.kind = OwnerKind::TU;
    o.condArmId = condArmId;
    return o;
  }

  static Owner Include(uint64_t includeId,
                       std::optional<uint64_t> condArmId = std::nullopt) {
    Owner o;
    o.kind = OwnerKind::Include;
    o.includeId = includeId;
    o.condArmId = condArmId;
    return o;
  }

  static Owner MacroInvocation(
      uint64_t macroInvocationId,
      std::optional<uint64_t> condArmId = std::nullopt) {
    Owner o;
    o.kind = OwnerKind::MacroInvocation;
    o.macroInvocationId = macroInvocationId;
    o.condArmId = condArmId;
    return o;
  }

  static Owner MacroDirective(
      uint64_t macroDirectiveId,
      std::optional<uint64_t> condArmId = std::nullopt) {
    Owner o;
    o.kind = OwnerKind::MacroDirective;
    o.macroDirectiveId = macroDirectiveId;
    o.condArmId = condArmId;
    return o;
  }

  static Owner LineControlIsland(
      uint64_t lineControlId,
      std::optional<uint64_t> condArmId = std::nullopt) {
    Owner o;
    o.kind = OwnerKind::LineControlIsland;
    o.lineControlId = lineControlId;
    o.condArmId = condArmId;
    return o;
  }

  static Owner PragmaIsland(
      uint64_t pragmaId,
      std::optional<uint64_t> condArmId = std::nullopt) {
    Owner o;
    o.kind = OwnerKind::PragmaIsland;
    o.pragmaId = pragmaId;
    o.condArmId = condArmId;
    return o;
  }

  static Owner ConditionalGroup(uint64_t condGroupId) {
    Owner o;
    o.kind = OwnerKind::ConditionalGroup;
    o.condGroupId = condGroupId;
    return o;
  }

  static Owner ConditionalArm(uint64_t condArmId) {
    Owner o;
    o.kind = OwnerKind::ConditionalArm;
    o.condArmId = condArmId;
    return o;
  }

  static Owner Unknown() { return Owner(); }

  bool IsKnown() const { return kind != OwnerKind::Unknown; }

  bool IsTU() const { return kind == OwnerKind::TU; }

  bool IsInclude() const { return kind == OwnerKind::Include; }

  bool IsMacroInvocation() const {
    return kind == OwnerKind::MacroInvocation;
  }

  bool IsMacroDirective() const {
    return kind == OwnerKind::MacroDirective;
  }

  bool IsLineControlIsland() const {
    return kind == OwnerKind::LineControlIsland;
  }

  bool IsPragmaIsland() const {
    return kind == OwnerKind::PragmaIsland;
  }

  bool IsConditionalGroup() const {
    return kind == OwnerKind::ConditionalGroup;
  }

  bool IsConditionalArm() const {
    return kind == OwnerKind::ConditionalArm;
  }

  bool HasSameIdentity(const Owner &other) const {
    return kind == other.kind && includeId == other.includeId &&
           macroInvocationId == other.macroInvocationId &&
           macroDirectiveId == other.macroDirectiveId &&
           lineControlId == other.lineControlId &&
           pragmaId == other.pragmaId && condGroupId == other.condGroupId &&
           condArmId == other.condArmId;
  }
};

/// Half-open token interval in either the original preprocessed token stream
/// A or the edited preprocessed token stream B.
struct OwnerTokenRange {
  uint64_t begin = 0;
  uint64_t end = 0;

  static OwnerTokenRange From(uint64_t begin, uint64_t end) {
    return {begin, end};
  }

  bool IsValid() const { return begin <= end; }

  bool Empty() const { return begin == end; }

  bool Contains(uint64_t tok) const { return begin <= tok && tok < end; }

  bool Contains(OwnerTokenRange other) const {
    return IsValid() && other.IsValid() && begin <= other.begin &&
           other.end <= end;
  }
};

/// Half-open physical source interval owned by one source surface.
///
/// `path` is the owner-local source file, `[begin,end)` is byte-based in that
/// file, and `includeId` identifies the concrete include occurrence when the
/// source surface is header-owned.  The range is only a candidate source
/// fact; `OwnerClosure::IsComplete()` is the theorem-facing gate that also
/// requires owner identity and token-envelope validity.
struct OwnerSourceRange {
  std::string path;
  uint64_t begin = 0;
  uint64_t end = 0;
  std::optional<uint64_t> includeId = std::nullopt;

  static OwnerSourceRange From(StringRef path, uint64_t begin, uint64_t end,
                               std::optional<uint64_t> includeId =
                                   std::nullopt) {
    OwnerSourceRange r;
    r.path = path.str();
    r.begin = begin;
    r.end = end;
    r.includeId = includeId;
    return r;
  }

  bool IsValid() const { return begin <= end; }

  bool Empty() const { return begin == end; }

  bool HasPath() const { return !path.empty(); }

  bool IsComplete() const { return HasPath() && IsValid(); }
};

struct OwnerObserverSummary;

/// Producer-derived identity for one macro-state fact.
///
/// Macro state remains component-specific instead of treating the entire
/// macro namespace as one global bit.  The identity deliberately stores only
/// producer facts or deterministic projections of those facts; it never
/// reparses a directive to infer missing state.  Empty optional fields mean
/// the producer did not supply that part of the identity, in which case the
/// owner summary also carries the appropriate conservative missing/unmodeled
/// marker.
struct MacroStateIdentity {
  std::string macroName;
  std::optional<uint64_t> definitionDirectiveId = std::nullopt;
  std::optional<uint64_t> undefDirectiveId = std::nullopt;
  bool functionLike = false;
  std::optional<uint32_t> arity = std::nullopt;
  bool variadic = false;
  std::string replacementTokenHash;

  bool operator==(const MacroStateIdentity &other) const {
    return macroName == other.macroName &&
           definitionDirectiveId == other.definitionDirectiveId &&
           undefDirectiveId == other.undefDirectiveId &&
           functionLike == other.functionLike && arity == other.arity &&
           variadic == other.variadic &&
           replacementTokenHash == other.replacementTokenHash;
  }
};

/// The kind of macro-state observation made by an owner.
///
/// Ordinary macro expansion, `defined(NAME)` queries, and
/// conditional-expression macro dependencies are separate observations. These
/// observations are not interchangeable: a suffix may depend only on whether
/// a macro is defined, on the replacement tokens used for expansion, or on
/// conditional branch selection.
enum class MacroObservationKind : uint8_t {
  Expansion,
  DefinedOperator,
  ConditionalEvaluation
};

inline StringRef toString(MacroObservationKind kind) {
  switch (kind) {
  case MacroObservationKind::Expansion:
    return "MacroExpansionObservation";
  case MacroObservationKind::DefinedOperator:
    return "DefinedOperatorObservation";
  case MacroObservationKind::ConditionalEvaluation:
    return "ConditionalMacroObservation";
  }
  llvm_unreachable("Invalid macro observation kind");
}

struct MacroStateObservation {
  MacroObservationKind kind = MacroObservationKind::Expansion;
  MacroStateIdentity identity;

  bool operator==(const MacroStateObservation &other) const {
    return kind == other.kind && identity == other.identity;
  }
};

/// Producer provenance for a logical line-control mutation.
///
/// Line/file state is component-specific.  A `#line` directive is not just
/// textual trivia: it mutates the logical line, file, and basename state
/// later observed by `__LINE__`, `__FILE__`, and `__FILE_NAME__`. These
/// provenance values say whether the producer, not the consumer, evaluated
/// the directive operands and conditional activity.
enum class LineDirectiveOperandProvenance : uint8_t {
  ProducerEvaluatedOperands,
  InactiveDirective,
  MissingProducerOperands,
  Unknown
};

inline StringRef toString(LineDirectiveOperandProvenance provenance) {
  switch (provenance) {
  case LineDirectiveOperandProvenance::ProducerEvaluatedOperands:
    return "ProducerEvaluatedOperands";
  case LineDirectiveOperandProvenance::InactiveDirective:
    return "InactiveDirective";
  case LineDirectiveOperandProvenance::MissingProducerOperands:
    return "MissingProducerOperands";
  case LineDirectiveOperandProvenance::Unknown:
    return "Unknown";
  }
  llvm_unreachable("Invalid line-directive operand provenance");
}

/// Producer-derived identity for one zero-token line-control state mutation.
///
/// The physical directive bytes may produce no ordinary A-side tokens, but the
/// event changes the logical source-location state observed by later owners.
/// Empty optional fields are conservative missing facts: later theorem checks
/// may reject the transition, but they must not infer it from downstream
/// `__LINE__`/`__FILE__` values.
struct LineControlStateIdentity {
  uint64_t eventId = 0;
  std::string physicalFile;
  std::optional<uint64_t> siteBegin = std::nullopt;
  std::optional<uint64_t> siteEnd = std::nullopt;
  bool active = false;
  bool producerProven = false;
  uint64_t logicalLineAfter = 0;
  std::string logicalFileAfter;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
  LineDirectiveOperandProvenance operandProvenance =
      LineDirectiveOperandProvenance::Unknown;

  bool operator==(const LineControlStateIdentity &other) const {
    return eventId == other.eventId && physicalFile == other.physicalFile &&
           siteBegin == other.siteBegin && siteEnd == other.siteEnd &&
           active == other.active && producerProven == other.producerProven &&
           logicalLineAfter == other.logicalLineAfter &&
           logicalFileAfter == other.logicalFileAfter &&
           ownerIncludeId == other.ownerIncludeId &&
           operandProvenance == other.operandProvenance;
  }
};

/// Location-state component observed by a builtin token.
enum class BuiltinLocationObservationKind : uint8_t {
  LineState,
  FileState,
  FileNameState
};

inline StringRef toString(BuiltinLocationObservationKind kind) {
  switch (kind) {
  case BuiltinLocationObservationKind::LineState:
    return "LineState";
  case BuiltinLocationObservationKind::FileState:
    return "FileState";
  case BuiltinLocationObservationKind::FileNameState:
    return "FileNameState";
  }
  llvm_unreachable("Invalid builtin location observation kind");
}

/// Producer/source-local observation of logical line/file state.
///
/// The observation may come from source text, an invocation spelling, or an
/// already-expanded PP token.  Optional source/token anchors are evidence
/// only; absence keeps the coarse observation bit conservative without
/// fabricating a stronger ordering fact.
struct BuiltinLocationObservation {
  BuiltinLocationObservationKind kind =
      BuiltinLocationObservationKind::LineState;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
  std::optional<uint64_t> sourceBegin = std::nullopt;
  std::optional<uint64_t> sourceEnd = std::nullopt;
  std::optional<uint64_t> aTokenBegin = std::nullopt;
  std::optional<uint64_t> aTokenEnd = std::nullopt;

  bool operator==(const BuiltinLocationObservation &other) const {
    return kind == other.kind && ownerIncludeId == other.ownerIncludeId &&
           sourceBegin == other.sourceBegin && sourceEnd == other.sourceEnd &&
           aTokenBegin == other.aTokenBegin && aTokenEnd == other.aTokenEnd;
  }
};

/// Producer-derived identity for one `__COUNTER__` event.
///
/// Counter state is event-specific.  The identity names the producer macro
/// invocation and concrete A-token occurrence that consumed the counter stream.
/// `expectedBValue` is optional because ordinary owner-state summaries are
/// edit-independent; stabilization code can fill or check it when an A->B
/// alignment is available.
struct CounterEventIdentity {
  uint64_t macroInvocationId = 0;
  uint64_t occurrenceOrdinal = 0;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
  std::optional<uint64_t> callerMacroId = std::nullopt;
  uint64_t aTokenBegin = 0;
  uint64_t aTokenEnd = 0;
  std::string expansionSiteFile;
  std::optional<uint64_t> expansionSiteBegin = std::nullopt;
  std::optional<uint64_t> expansionSiteEnd = std::nullopt;
  std::string aValue;
  std::optional<std::string> expectedBValue = std::nullopt;
  bool canStabilizeByLiteralization = false;
  bool canStabilizeByMaterialization = false;

  bool operator==(const CounterEventIdentity &other) const {
    return macroInvocationId == other.macroInvocationId &&
           occurrenceOrdinal == other.occurrenceOrdinal &&
           ownerIncludeId == other.ownerIncludeId &&
           callerMacroId == other.callerMacroId &&
           aTokenBegin == other.aTokenBegin && aTokenEnd == other.aTokenEnd &&
           expansionSiteFile == other.expansionSiteFile &&
           expansionSiteBegin == other.expansionSiteBegin &&
           expansionSiteEnd == other.expansionSiteEnd &&
           aValue == other.aValue && expectedBValue == other.expectedBValue &&
           canStabilizeByLiteralization ==
               other.canStabilizeByLiteralization &&
           canStabilizeByMaterialization ==
               other.canStabilizeByMaterialization;
  }
};


/// Producer-derived identity for a concrete include directive occurrence.
///
/// Include-path resolution, include file identity, include token
/// materialization, and include-state effects are separate facts.  This
/// identity is built only from serialized producer facts: the as-written
/// target, the resolved path when present, the includer/source site, and the
/// concrete include instance id.  It is not an include-path resolver.
struct IncludeStateIdentity {
  uint64_t includeId = 0;
  std::string directiveKind;
  std::string sitePath;
  uint64_t siteBegin = 0;
  uint64_t siteEnd = 0;
  std::string target;
  std::optional<std::string> resolvedPath = std::nullopt;
  bool angled = false;
  std::optional<uint64_t> parentIncludeId = std::nullopt;
  bool hasTokenMaterialization = false;

  bool operator==(const IncludeStateIdentity &other) const {
    return includeId == other.includeId &&
           directiveKind == other.directiveKind &&
           sitePath == other.sitePath && siteBegin == other.siteBegin &&
           siteEnd == other.siteEnd && target == other.target &&
           resolvedPath == other.resolvedPath && angled == other.angled &&
           parentIncludeId == other.parentIncludeId &&
           hasTokenMaterialization == other.hasTokenMaterialization;
  }
};

/// The producer-visible include-guard role of one include occurrence.
///
/// The current map does not yet serialize a first-class include-guard oracle,
/// so `guardMacroName` is optional.  An empty guard macro is still useful: it
/// ties the guard-state risk to a concrete header/include identity instead of
/// poisoning the whole include-state universe.  Later producer extensions can
/// fill the macro name without changing the theorem-facing state shape.
enum class IncludeGuardObservationKind : uint8_t {
  ActiveIncludeMayMutateGuard,
  SkippedIncludeMayObserveGuard,
  UnknownGuardEffect
};

inline StringRef toString(IncludeGuardObservationKind kind) {
  switch (kind) {
  case IncludeGuardObservationKind::ActiveIncludeMayMutateGuard:
    return "ActiveIncludeMayMutateGuard";
  case IncludeGuardObservationKind::SkippedIncludeMayObserveGuard:
    return "SkippedIncludeMayObserveGuard";
  case IncludeGuardObservationKind::UnknownGuardEffect:
    return "UnknownGuardEffect";
  }
  llvm_unreachable("Invalid include-guard observation kind");
}

struct IncludeGuardStateIdentity {
  IncludeGuardObservationKind kind =
      IncludeGuardObservationKind::UnknownGuardEffect;
  uint64_t includeId = 0;
  std::string headerPath;
  std::optional<std::string> guardMacroName = std::nullopt;
  std::optional<uint64_t> parentIncludeId = std::nullopt;
  bool producerProvenGuard = false;

  bool operator==(const IncludeGuardStateIdentity &other) const {
    return kind == other.kind && includeId == other.includeId &&
           headerPath == other.headerPath &&
           guardMacroName == other.guardMacroName &&
           parentIncludeId == other.parentIncludeId &&
           producerProvenGuard == other.producerProvenGuard;
  }
};

/// Producer-side pragma classification used by suffix-state proof code.
///
/// Unknown pragmas remain fail-closed.  Known diagnostic pragmas are recorded
/// as component-specific state so later suffix-stability code can distinguish
/// a balanced/local pragma island from an opaque state transition.
enum class PragmaStateClassification : uint8_t {
  KnownLocalPragmaState,
  KnownBalancedPragmaState,
  UnknownPragmaState
};

inline StringRef toString(PragmaStateClassification classification) {
  switch (classification) {
  case PragmaStateClassification::KnownLocalPragmaState:
    return "KnownLocalPragmaState";
  case PragmaStateClassification::KnownBalancedPragmaState:
    return "KnownBalancedPragmaState";
  case PragmaStateClassification::UnknownPragmaState:
    return "UnknownPragmaState";
  }
  llvm_unreachable("Invalid pragma-state classification");
}

struct PragmaStateIdentity {
  uint64_t pragmaId = 0;
  std::string sitePath;
  uint64_t siteBegin = 0;
  uint64_t siteEnd = 0;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
  PragmaStateClassification classification =
      PragmaStateClassification::UnknownPragmaState;
  std::string directiveFingerprint;

  bool operator==(const PragmaStateIdentity &other) const {
    return pragmaId == other.pragmaId && sitePath == other.sitePath &&
           siteBegin == other.siteBegin && siteEnd == other.siteEnd &&
           ownerIncludeId == other.ownerIncludeId &&
           classification == other.classification &&
           directiveFingerprint == other.directiveFingerprint;
  }
};

/// Producer-derived branch-selection state for a conditional group or arm.
///
/// Conditional selection is explicit state rather than just selected source
/// text.  The identity records which directive/arm was producer-selected and
/// which condition text was observed.  It does not reverse-solve a different
/// branch condition from downstream B-side tokens.
enum class ConditionalStateRole : uint8_t {
  ConditionalGroup,
  ActiveArm,
  InactiveArm
};

inline StringRef toString(ConditionalStateRole role) {
  switch (role) {
  case ConditionalStateRole::ConditionalGroup:
    return "ConditionalGroup";
  case ConditionalStateRole::ActiveArm:
    return "ActiveArm";
  case ConditionalStateRole::InactiveArm:
    return "InactiveArm";
  }
  llvm_unreachable("Invalid conditional-state role");
}

struct ConditionalStateIdentity {
  ConditionalStateRole role = ConditionalStateRole::ConditionalGroup;
  uint64_t groupId = 0;
  std::optional<uint64_t> armId = std::nullopt;
  std::string file;
  uint64_t groupBegin = 0;
  uint64_t groupEnd = 0;
  std::optional<uint64_t> parentArmId = std::nullopt;
  std::optional<uint64_t> parentIncludeId = std::nullopt;
  std::string armKind;
  std::optional<std::string> conditionText = std::nullopt;
  bool selected = false;
  std::optional<uint64_t> aTokenBegin = std::nullopt;
  std::optional<uint64_t> aTokenEnd = std::nullopt;
  bool conditionTruthProducerProven = true;
  bool reverseSolvedDirectiveRequired = false;

  bool operator==(const ConditionalStateIdentity &other) const {
    return role == other.role && groupId == other.groupId &&
           armId == other.armId && file == other.file &&
           groupBegin == other.groupBegin && groupEnd == other.groupEnd &&
           parentArmId == other.parentArmId &&
           parentIncludeId == other.parentIncludeId &&
           armKind == other.armKind && conditionText == other.conditionText &&
           selected == other.selected && aTokenBegin == other.aTokenBegin &&
           aTokenEnd == other.aTokenEnd &&
           conditionTruthProducerProven ==
               other.conditionTruthProducerProven &&
           reverseSolvedDirectiveRequired ==
               other.reverseSolvedDirectiveRequired;
  }
};

/// Component-specific producer fact that is missing from the owner-state
/// summary.
///
/// Precise missing-fact markers let theorem-facing code distinguish a missing
/// macro definition identity from missing line-control operands, counter events,
/// pragma classification, include-guard facts, conditional branch facts, or
/// owner ordering.
enum class MissingStateFactKind : uint8_t {
  MissingMacroFacts,
  MissingLineControlFacts,
  MissingCounterFacts,
  MissingPragmaFacts,
  MissingIncludeGuardFacts,
  MissingConditionalFacts,
  MissingOwnerOrderingFacts
};

inline StringRef toString(MissingStateFactKind kind) {
  switch (kind) {
  case MissingStateFactKind::MissingMacroFacts:
    return "MissingMacroFacts";
  case MissingStateFactKind::MissingLineControlFacts:
    return "MissingLineControlFacts";
  case MissingStateFactKind::MissingCounterFacts:
    return "MissingCounterFacts";
  case MissingStateFactKind::MissingPragmaFacts:
    return "MissingPragmaFacts";
  case MissingStateFactKind::MissingIncludeGuardFacts:
    return "MissingIncludeGuardFacts";
  case MissingStateFactKind::MissingConditionalFacts:
    return "MissingConditionalFacts";
  case MissingStateFactKind::MissingOwnerOrderingFacts:
    return "MissingOwnerOrderingFacts";
  }
  llvm_unreachable("Invalid missing state fact kind");
}

struct MissingStateFact {
  MissingStateFactKind kind = MissingStateFactKind::MissingOwnerOrderingFacts;
  std::string detail;

  bool operator==(const MissingStateFact &other) const {
    return kind == other.kind && detail == other.detail;
  }
};

/// Shared state-fact payload.
///
/// Every fact in this bucket is component-specific: macro facts are keyed by
/// macro identity, line-control facts are keyed by producer events, counter
/// facts are keyed by event identity, and missing producer information is
/// carried explicitly as a MissingStateFact obligation.
struct OwnerStateFacts {
  // component-specific macro facts.
  std::vector<MacroStateIdentity> macroDefinitions;
  std::vector<MacroStateIdentity> macroUndefinitions;
  std::vector<MacroStateIdentity> macroRequirements;
  std::vector<MacroStateObservation> macroExpansionObservations;
  std::vector<MacroStateObservation> definedOperatorObservations;
  std::vector<MacroStateObservation> conditionalMacroObservations;

  // component-specific line-control facts.  `lineControlEvents` are
  // zero-token state mutations; `builtinLocationObservations` are reads of a
  // particular logical-location component.
  std::vector<LineControlStateIdentity> lineControlEvents;
  std::vector<BuiltinLocationObservation> builtinLocationObservations;

  // component-specific counter events.  Each event names one producer-proven
  // occurrence instead of treating the counter stream as a global text scan.
  std::vector<CounterEventIdentity> counterEvents;

  // component-specific pragma facts.  Unknown pragma semantics stay attached
  // to the event identity instead of a global poison bit.
  std::vector<PragmaStateIdentity> pragmaStateEvents;

  // component-specific include facts.  These keep include pathcresolution,
  // file identity, token materialization, and guard effects separable.
  std::vector<IncludeStateIdentity> includeStateEvents;
  std::vector<IncludeGuardStateIdentity> includeGuardStateEvents;

  // component-specific conditional facts.  These distinguish the directive
  // island, active arm, inactive arms, condition-expression macro
  // dependencies, and reverse-solving boundary explicitly.
  std::vector<ConditionalStateIdentity> conditionalStateEvents;

  // component-specific missing producer facts.  These are theorem-facing
  // obligations; incomplete facts make composition less admissible, never
  // more admissible.
  std::vector<MissingStateFact> missingStateFacts;

  bool HasMissingStateFacts() const { return !missingStateFacts.empty(); }

  bool HasMissingFactKind(MissingStateFactKind kind) const {
    return llvm::any_of(missingStateFacts, [&](const MissingStateFact &fact) {
      return fact.kind == kind;
    });
  }

  bool HasMacroDefinitions() const { return !macroDefinitions.empty(); }

  bool HasMacroUndefinitions() const { return !macroUndefinitions.empty(); }

  bool HasMacroRequirements() const { return !macroRequirements.empty(); }

  bool HasMacroExpansionObservations() const {
    return !macroExpansionObservations.empty();
  }

  bool HasDefinedOperatorObservations() const {
    return !definedOperatorObservations.empty();
  }

  bool HasConditionalMacroObservations() const {
    return !conditionalMacroObservations.empty();
  }

  bool HasLineControlEvents() const { return !lineControlEvents.empty(); }

  bool HasBuiltinLocationObservations() const {
    return !builtinLocationObservations.empty();
  }

  bool HasCounterEvents() const { return !counterEvents.empty(); }

  bool HasPragmaStateEvents() const { return !pragmaStateEvents.empty(); }

  bool HasIncludeStateEvents() const { return !includeStateEvents.empty(); }

  bool HasIncludeGuardStateEvents() const {
    return !includeGuardStateEvents.empty();
  }

  bool HasConditionalStateEvents() const {
    return !conditionalStateEvents.empty();
  }

  /// Return true if this bucket contains theorem-facing component facts.
  bool HasTheoremStateFacts() const {
    return HasMacroDefinitions() || HasMacroUndefinitions() ||
           HasMacroRequirements() || HasMacroExpansionObservations() ||
           HasDefinedOperatorObservations() ||
           HasConditionalMacroObservations() || HasLineControlEvents() ||
           HasBuiltinLocationObservations() || HasCounterEvents() ||
           HasPragmaStateEvents() || HasIncludeStateEvents() ||
           HasIncludeGuardStateEvents() || HasConditionalStateEvents() ||
           HasMissingStateFacts();
  }

  /// Copy theorem-facing component facts from `other`.
  OwnerStateFacts &MergeTheoremFactsFrom(const OwnerStateFacts &other) {
    for (const MacroStateIdentity &identity : other.macroDefinitions)
      AddMacroDefinition(identity);
    for (const MacroStateIdentity &identity : other.macroUndefinitions)
      AddMacroUndefinition(identity);
    for (const MacroStateIdentity &identity : other.macroRequirements)
      AddMacroRequirement(identity);
    for (const MacroStateObservation &observation :
         other.macroExpansionObservations)
      AddMacroObservation(observation);
    for (const MacroStateObservation &observation :
         other.definedOperatorObservations)
      AddMacroObservation(observation);
    for (const MacroStateObservation &observation :
         other.conditionalMacroObservations)
      AddMacroObservation(observation);
    for (const LineControlStateIdentity &identity :
         other.lineControlEvents)
      AddLineControlEvent(identity);
    for (const BuiltinLocationObservation &observation :
         other.builtinLocationObservations)
      AddBuiltinLocationObservation(observation);
    for (const CounterEventIdentity &identity : other.counterEvents)
      AddCounterEvent(identity);
    for (const PragmaStateIdentity &identity : other.pragmaStateEvents)
      AddPragmaStateEvent(identity);
    for (const IncludeStateIdentity &identity : other.includeStateEvents)
      AddIncludeStateEvent(identity);
    for (const IncludeGuardStateIdentity &identity :
         other.includeGuardStateEvents)
      AddIncludeGuardStateEvent(identity);
    for (const ConditionalStateIdentity &identity :
         other.conditionalStateEvents)
      AddConditionalStateEvent(identity);
    for (const MissingStateFact &fact : other.missingStateFacts)
      AddMissingStateFact(fact.kind, fact.detail);
    return *this;
  }

  bool HasTheoremUnknownPragmaState() const {
    if (HasMissingFactKind(MissingStateFactKind::MissingPragmaFacts))
      return true;
    return llvm::any_of(
        pragmaStateEvents, [](const PragmaStateIdentity &identity) {
          return identity.classification ==
                 PragmaStateClassification::UnknownPragmaState;
        });
  }

  bool MutatesMacroState() const {
    return HasMacroDefinitions() || HasMacroUndefinitions();
  }

  bool ObservesBuiltinLocationState() const {
    return HasBuiltinLocationObservations();
  }

  bool MutatesLineFileState() const { return HasLineControlEvents(); }

  bool MutatesPragmaState() const { return HasPragmaStateEvents(); }

  bool MutatesIncludeState() const { return HasIncludeStateEvents(); }

  bool ObservesIncludeState() const { return HasIncludeStateEvents(); }

  bool MutatesIncludeGuardState() const {
    return HasIncludeGuardStateEvents();
  }

  bool ObservesIncludeGuardState() const {
    return HasIncludeGuardStateEvents();
  }

  bool MutatesConditionalState() const {
    return HasConditionalStateEvents();
  }

  bool ObservesConditionalState() const {
    return HasConditionalStateEvents();
  }

  bool MutatesAnyState() const {
    return MutatesMacroState() || MutatesLineFileState() ||
           HasCounterEvents() || MutatesPragmaState() ||
           MutatesIncludeState() || MutatesIncludeGuardState() ||
           MutatesConditionalState() || HasMissingStateFacts();
  }

  bool ObservesAnyState() const {
    return HasMacroRequirements() || HasMacroExpansionObservations() ||
           HasDefinedOperatorObservations() ||
           HasConditionalMacroObservations() ||
           ObservesBuiltinLocationState() || HasCounterEvents() ||
           HasPragmaStateEvents() || ObservesIncludeState() ||
           ObservesIncludeGuardState() || ObservesConditionalState() ||
           HasMissingStateFacts();
  }

  bool Empty() const { return !MutatesAnyState() && !ObservesAnyState(); }

  template <typename T>
  static void AppendUnique(std::vector<T> &dst, ArrayRef<T> src) {
    for (const T &item : src)
      AppendUniqueOne(dst, item);
  }

  template <typename T>
  static void AppendUniqueOne(std::vector<T> &dst, const T &item) {
    if (llvm::none_of(dst, [&](const T &existing) {
          return existing == item;
        }))
      dst.push_back(item);
  }

  void AddMissingStateFact(MissingStateFactKind kind, StringRef detail) {
    MissingStateFact fact;
    fact.kind = kind;
    fact.detail = detail.str();
    AppendUniqueOne(missingStateFacts, fact);
  }

  void AddMacroDefinition(const MacroStateIdentity &identity) {
    AppendUniqueOne(macroDefinitions, identity);
  }

  void AddMacroUndefinition(const MacroStateIdentity &identity) {
    AppendUniqueOne(macroUndefinitions, identity);
  }

  void AddMacroRequirement(const MacroStateIdentity &identity) {
    AppendUniqueOne(macroRequirements, identity);
  }

  void AddMacroObservation(const MacroStateObservation &observation) {
    switch (observation.kind) {
    case MacroObservationKind::Expansion:
      AppendUniqueOne(macroExpansionObservations, observation);
      break;
    case MacroObservationKind::DefinedOperator:
      AppendUniqueOne(definedOperatorObservations, observation);
      break;
    case MacroObservationKind::ConditionalEvaluation:
      AppendUniqueOne(conditionalMacroObservations, observation);
      break;
    }
  }

  void AddLineControlEvent(const LineControlStateIdentity &identity) {
    AppendUniqueOne(lineControlEvents, identity);
  }

  void AddBuiltinLocationObservation(
      const BuiltinLocationObservation &observation) {
    AppendUniqueOne(builtinLocationObservations, observation);
  }

  void AddCounterObservation(const CounterEventIdentity &identity) {
    AppendUniqueOne(counterEvents, identity);
  }

  void AddCounterMutation(const CounterEventIdentity &identity) {
    AppendUniqueOne(counterEvents, identity);
  }

  void AddCounterEvent(const CounterEventIdentity &identity) {
    AddCounterObservation(identity);
  }

  void AddPragmaStateEvent(const PragmaStateIdentity &identity) {
    AppendUniqueOne(pragmaStateEvents, identity);
  }

  void AddIncludeStateEvent(const IncludeStateIdentity &identity) {
    AppendUniqueOne(includeStateEvents, identity);
  }

  void AddIncludeGuardStateEvent(
      const IncludeGuardStateIdentity &identity) {
    AppendUniqueOne(includeGuardStateEvents, identity);
    // Missing guard-oracle facts are represented in the identity itself.
    // will convert component-specific missing facts into named
    // obligations; do not collapse them back into the global unmodeled bit
    // here, or every include would conservatively poison unrelated state.
  }

  void AddConditionalStateEvent(
      const ConditionalStateIdentity &identity) {
    AppendUniqueOne(conditionalStateEvents, identity);
  }

  OwnerStateFacts &MergeFrom(const OwnerStateFacts &other) {
    AppendUnique(macroDefinitions,
                 ArrayRef<MacroStateIdentity>(other.macroDefinitions));
    AppendUnique(macroUndefinitions,
                 ArrayRef<MacroStateIdentity>(other.macroUndefinitions));
    AppendUnique(macroRequirements,
                 ArrayRef<MacroStateIdentity>(other.macroRequirements));
    AppendUnique(macroExpansionObservations,
                 ArrayRef<MacroStateObservation>(
                     other.macroExpansionObservations));
    AppendUnique(definedOperatorObservations,
                 ArrayRef<MacroStateObservation>(
                     other.definedOperatorObservations));
    AppendUnique(conditionalMacroObservations,
                 ArrayRef<MacroStateObservation>(
                     other.conditionalMacroObservations));
    AppendUnique(lineControlEvents,
                 ArrayRef<LineControlStateIdentity>(other.lineControlEvents));
    AppendUnique(builtinLocationObservations,
                 ArrayRef<BuiltinLocationObservation>(
                     other.builtinLocationObservations));
    AppendUnique(counterEvents,
                 ArrayRef<CounterEventIdentity>(other.counterEvents));
    AppendUnique(pragmaStateEvents,
                 ArrayRef<PragmaStateIdentity>(other.pragmaStateEvents));
    AppendUnique(includeStateEvents,
                 ArrayRef<IncludeStateIdentity>(other.includeStateEvents));
    AppendUnique(includeGuardStateEvents,
                 ArrayRef<IncludeGuardStateIdentity>(
                     other.includeGuardStateEvents));
    AppendUnique(conditionalStateEvents,
                 ArrayRef<ConditionalStateIdentity>(
                     other.conditionalStateEvents));
    AppendUnique(missingStateFacts,
                 ArrayRef<MissingStateFact>(other.missingStateFacts));
    return *this;
  }
};

/// State required to hold at owner entry before the owner's source bytes are
/// replayed or preserved.  The payload is already component-specific through
/// OwnerStateFacts; callers should add precise facts rather than
/// reintroducing coarse owner-wide booleans.
struct StateRequirements : OwnerStateFacts {};

/// State actually observed by the owner while producing its A-token envelope.
struct StateObservations : OwnerStateFacts {};

/// State transitions performed by the owner.
struct StateMutations : OwnerStateFacts {};

/// State equivalences the owner requires at exit for a preserved suffix to
/// remain valid.  Today this is projected from the mutation surface; later
/// proof passes can record finer post-state guarantees here.
struct StateGuarantees : OwnerStateFacts {};

/// Canonical state-delta model for one owner.
///
/// This is the theorem-facing answer to four different questions that the old
/// boolean-only summary could not separate:
///
///   * Entry:    what state must already be true before this owner?
///   * Observes: what state does this owner read?
///   * Mutates:  what state does this owner change?
///   * Exit:     what state must be equivalent after this owner?
///
/// The buckets are monotone.  A missing producer fact must set the relevant
/// unmodeled bit in one or more buckets, never clear an obligation.
struct OwnerStateDelta {
  StateRequirements Entry;
  StateObservations Observes;
  StateMutations Mutates;
  StateGuarantees Exit;

  bool Empty() const {
    return Entry.Empty() && Observes.Empty() && Mutates.Empty() &&
           Exit.Empty();
  }

  OwnerStateDelta &MergeFrom(const OwnerStateDelta &other) {
    Entry.MergeFrom(other.Entry);
    Observes.MergeFrom(other.Observes);
    Mutates.MergeFrom(other.Mutates);
    Exit.MergeFrom(other.Exit);
    return *this;
  }

  /// Merge theorem-facing component facts from another delta.
  ///
  /// This path keeps the Entry/Observes/Mutates/Exit bucket structure intact
  /// while preserving the precise per-component facts already attached to the
  /// source delta.
  OwnerStateDelta &MergeTheoremFactsFrom(const OwnerStateDelta &other) {
    Entry.MergeTheoremFactsFrom(other.Entry);
    Observes.MergeTheoremFactsFrom(other.Observes);
    Mutates.MergeTheoremFactsFrom(other.Mutates);
    Exit.MergeTheoremFactsFrom(other.Exit);
    return *this;
  }
};

/// Owner closures now carry OwnerStateDelta directly.  OwnerStateFacts remains
/// as the builder bucket for producer-derived component facts; it is not a
/// theorem-facing carrier on its own.

/// Summary of suffix-visible observers that constrain state composition.
///
/// These are the observable surfaces for suffix-stability proofs.  The model
/// is intentionally selective: a state component only constrains composition
/// when a later preserved owner actually observes it.
struct OwnerObserverSummary {
  bool observesMacroExpansion = false;
  bool observesDefinedOperator = false;
  bool observesConditionalEvaluation = false;
  bool observesLineNumber = false;
  bool observesFileState = false;
  bool observesFileName = false;
  bool observesCounter = false;
  bool observesPragmaState = false;
  bool observesIncludeGuardState = false;
  bool observesIncludeState = false;

  bool Empty() const {
    return !observesMacroExpansion && !observesDefinedOperator &&
           !observesConditionalEvaluation && !observesLineNumber &&
           !observesFileState && !observesFileName && !observesCounter &&
           !observesPragmaState &&
           !observesIncludeGuardState && !observesIncludeState;
  }

  OwnerObserverSummary &MergeFrom(const OwnerObserverSummary &other) {
    observesMacroExpansion |= other.observesMacroExpansion;
    observesDefinedOperator |= other.observesDefinedOperator;
    observesConditionalEvaluation |= other.observesConditionalEvaluation;
    observesLineNumber |= other.observesLineNumber;
    observesFileState |= other.observesFileState;
    observesFileName |= other.observesFileName;
    observesCounter |= other.observesCounter;
    observesPragmaState |= other.observesPragmaState;
    observesIncludeGuardState |= other.observesIncludeGuardState;
    observesIncludeState |= other.observesIncludeState;
    return *this;
  }
};

/// Canonical theorem-facing closure record for one owner-local refolding
/// candidate.
///
/// This is the normalization point for closed-domain owner proofs:
/// TU edits, include/header realizations, macro invocation rewrites,
/// conditional-arm islands, sideband line/pragmas, and future mixed-owner
/// tiling segments should all be expressible as an `OwnerClosure` before they
/// are admitted by a proof gate.  The structure is intentionally passive in
/// this representation; it gives the existing specialized machinery a common
/// target without changing emission behavior.
struct OwnerClosure {
  Owner owner;
  OwnerSourceRange source;
  OwnerTokenRange aTokens;
  OwnerTokenRange bTokens;
  OwnerStateDelta stateIn;
  OwnerStateDelta stateOut;
  OwnerObserverSummary observers;

  static OwnerClosure From(Owner owner, OwnerSourceRange source,
                           OwnerTokenRange aTokens,
                           OwnerTokenRange bTokens) {
    OwnerStateDelta stateIn;
    OwnerStateDelta stateOut;
    OwnerObserverSummary observers;
    return From(std::move(owner), std::move(source), aTokens, bTokens,
                stateIn, stateOut, observers);
  }

  static OwnerClosure From(Owner owner, OwnerSourceRange source,
                           OwnerTokenRange aTokens,
                           OwnerTokenRange bTokens,
                           OwnerStateDelta stateIn,
                           OwnerStateDelta stateOut,
                           OwnerObserverSummary observers) {
    OwnerClosure closure;
    closure.owner = std::move(owner);
    closure.source = std::move(source);
    closure.aTokens = aTokens;
    closure.bTokens = bTokens;
    closure.stateIn = stateIn;
    closure.stateOut = stateOut;
    closure.observers = observers;
    return closure;
  }

  bool IsComplete() const {
    return owner.IsKnown() && source.IsComplete() && aTokens.IsValid() &&
           bTokens.IsValid();
  }

  bool IsStateNeutral() const {
    return stateIn.Empty() && stateOut.Empty() && observers.Empty();
  }

  bool HasSameOwner(const OwnerClosure &other) const {
    return owner.HasSameIdentity(other.owner);
  }
};

/// State component key used by the suffix-observer graph.
///
/// These components are intentionally coarser than Clang's full preprocessor
/// state.  They are the theorem-facing equivalence classes that an edit may
/// disturb before a preserved suffix: later enforcement only needs to know
/// whether a suffix owner can observe one of these components.
enum class OwnerStateComponent : uint8_t {
  MacroState,
  DefinedOperator,
  ConditionalState,
  LineNumber,
  FileState,
  FileName,
  Counter,
  PragmaState,
  IncludeGuardState,
  IncludeState,
  UnmodeledState,
  Unknown
};

inline StringRef toString(OwnerStateComponent component) {
  switch (component) {
  case OwnerStateComponent::MacroState:
    return "MacroState";
  case OwnerStateComponent::DefinedOperator:
    return "DefinedOperator";
  case OwnerStateComponent::ConditionalState:
    return "ConditionalState";
  case OwnerStateComponent::LineNumber:
    return "LineNumber";
  case OwnerStateComponent::FileState:
    return "FileState";
  case OwnerStateComponent::FileName:
    return "FileName";
  case OwnerStateComponent::Counter:
    return "Counter";
  case OwnerStateComponent::PragmaState:
    return "PragmaState";
  case OwnerStateComponent::IncludeGuardState:
    return "IncludeGuardState";
  case OwnerStateComponent::IncludeState:
    return "IncludeState";
  case OwnerStateComponent::UnmodeledState:
    return "UnmodeledState";
  case OwnerStateComponent::Unknown:
    return "Unknown";
  }
  llvm_unreachable("Invalid owner state component");
}

/// Boundary before a preserved suffix whose state observers are being queried.
///
/// A boundary may be source-based, token-based, or both.  Source boundaries
/// are used for owner-local repair decisions (`#define`, `#line`, pragmas,
/// etc.); token boundaries let future proof code ask about observers that are
/// only comparable through the A-token stream.  Missing comparison facts are
/// conservative: the query reports incomparable observers instead of assuming
/// there is no suffix dependency.
struct OwnerStateBoundary {
  OwnerSourceRange source;
  OwnerTokenRange aTokens;
  bool hasSourceBoundary = false;
  bool hasTokenBoundary = false;

  static OwnerStateBoundary FromSource(OwnerSourceRange source) {
    OwnerStateBoundary boundary;
    boundary.source = std::move(source);
    boundary.hasSourceBoundary = true;
    return boundary;
  }

  static OwnerStateBoundary FromATokens(OwnerTokenRange aTokens) {
    OwnerStateBoundary boundary;
    boundary.aTokens = aTokens;
    boundary.hasTokenBoundary = true;
    return boundary;
  }

  static OwnerStateBoundary FromSourceAndATokens(OwnerSourceRange source,
                                                 OwnerTokenRange aTokens) {
    OwnerStateBoundary boundary;
    boundary.source = std::move(source);
    boundary.aTokens = aTokens;
    boundary.hasSourceBoundary = true;
    boundary.hasTokenBoundary = true;
    return boundary;
  }
};

/// Canonical preprocessing-order node kinds for the state graph.
///
/// These are theorem-facing semantic events, not construction algorithms.
/// Ordinary token owners and macro invocations may have non-empty A-token
/// ranges; directive/control events often have empty token ranges but still
/// mutate or observe preprocessor state and therefore must remain ordered in
/// the graph.
enum class OwnerStateGraphNodeKind : uint8_t {
  OrdinaryTokenOwner,
  MacroInvocation,
  NestedMacroExpansion,
  IncludeEntry,
  IncludeExit,
  MacroDefineEvent,
  MacroUndefEvent,
  LineControlEvent,
  PragmaEvent,
  ConditionalEvent,
  CounterEvent,
  Unknown
};

inline StringRef toString(OwnerStateGraphNodeKind kind) {
  switch (kind) {
  case OwnerStateGraphNodeKind::OrdinaryTokenOwner:
    return "OrdinaryTokenOwner";
  case OwnerStateGraphNodeKind::MacroInvocation:
    return "MacroInvocation";
  case OwnerStateGraphNodeKind::NestedMacroExpansion:
    return "NestedMacroExpansion";
  case OwnerStateGraphNodeKind::IncludeEntry:
    return "IncludeEntry";
  case OwnerStateGraphNodeKind::IncludeExit:
    return "IncludeExit";
  case OwnerStateGraphNodeKind::MacroDefineEvent:
    return "MacroDefineEvent";
  case OwnerStateGraphNodeKind::MacroUndefEvent:
    return "MacroUndefEvent";
  case OwnerStateGraphNodeKind::LineControlEvent:
    return "LineControlEvent";
  case OwnerStateGraphNodeKind::PragmaEvent:
    return "PragmaEvent";
  case OwnerStateGraphNodeKind::ConditionalEvent:
    return "ConditionalEvent";
  case OwnerStateGraphNodeKind::CounterEvent:
    return "CounterEvent";
  case OwnerStateGraphNodeKind::Unknown:
    return "Unknown";
  }
  llvm_unreachable("Invalid owner state graph node kind");
}

/// Stable node in the persistent owner/state-event graph.
///
/// require every semantic event to be represented explicitly, including
/// zero-token directives such as #define, #undef, #line, #pragma,
/// conditional-control islands, include entry/exit markers, and __COUNTER__
/// events.  `closure` names the owner and source/token evidence; `state` is
/// the precise theorem-facing delta attached to that node.  `predecessor` and
/// `successor` are the deterministic total-order neighbors in this graph,
/// while later code may still reject nodes whose producer facts are
/// insufficient to compare against a particular edit boundary.
struct OwnerStateGraphNode {
  uint64_t id = 0;
  uint64_t order = 0;
  OwnerStateGraphNodeKind kind = OwnerStateGraphNodeKind::Unknown;
  OwnerClosure closure;
  OwnerStateDelta state;
  OwnerSourceRange source;
  OwnerTokenRange aTokens;
  std::optional<uint64_t> predecessor = std::nullopt;
  std::optional<uint64_t> successor = std::nullopt;
  std::optional<uint64_t> containingIncludeId = std::nullopt;
  std::optional<uint64_t> containingMacroInvocationId = std::nullopt;
  std::optional<uint64_t> containingConditionalArmId = std::nullopt;
  std::string detail;

  bool HasTokenAnchor() const {
    return aTokens.IsValid() && !aTokens.Empty();
  }

  bool IsZeroTokenEvent() const { return !HasTokenAnchor(); }
};

/// Observer kind returned by first-observer suffix queries.
///
/// The state component stays coarse enough for the state-transition gateway,
/// but records the exact observation surface that caused the suffix
/// dependency.  This lets repair/materialization code distinguish, for
/// example, ordinary macro expansion from defined(NAME), or __FILE__ from
/// __FILE_NAME__, without rescanning the owner.
enum class SuffixObservationKind : uint8_t {
  MacroExpansionObservation,
  DefinedOperatorObservation,
  ConditionalMacroObservation,
  LineStateObservation,
  FileStateObservation,
  FileNameStateObservation,
  CounterObservation,
  PragmaStateObservation,
  IncludeGuardStateObservation,
  IncludeStateObservation,
  UnmodeledStateObservation,
  Unknown
};

inline StringRef toString(SuffixObservationKind kind) {
  switch (kind) {
  case SuffixObservationKind::MacroExpansionObservation:
    return "MacroExpansionObservation";
  case SuffixObservationKind::DefinedOperatorObservation:
    return "DefinedOperatorObservation";
  case SuffixObservationKind::ConditionalMacroObservation:
    return "ConditionalMacroObservation";
  case SuffixObservationKind::LineStateObservation:
    return "LineStateObservation";
  case SuffixObservationKind::FileStateObservation:
    return "FileStateObservation";
  case SuffixObservationKind::FileNameStateObservation:
    return "FileNameStateObservation";
  case SuffixObservationKind::CounterObservation:
    return "CounterObservation";
  case SuffixObservationKind::PragmaStateObservation:
    return "PragmaStateObservation";
  case SuffixObservationKind::IncludeGuardStateObservation:
    return "IncludeGuardStateObservation";
  case SuffixObservationKind::IncludeStateObservation:
    return "IncludeStateObservation";
  case SuffixObservationKind::UnmodeledStateObservation:
    return "UnmodeledStateObservation";
  case SuffixObservationKind::Unknown:
    return "Unknown";
  }
  llvm_unreachable("Invalid suffix observation kind");
}

/// Proof that a suffix observer was ordered relative to an edit boundary.
///
/// Incomparable observer order is a named missing-producer-facts condition,
/// never absence of observation.  consumes this value to decide whether state
/// repair may be inserted before the first observer.
enum class SuffixOrderingProofKind : uint8_t {
  SourceOrder,
  TokenOrder,
  SourceAndTokenOrder,
  IncomparableMissingProducerFacts,
  Unknown
};

inline StringRef toString(SuffixOrderingProofKind kind) {
  switch (kind) {
  case SuffixOrderingProofKind::SourceOrder:
    return "SourceOrder";
  case SuffixOrderingProofKind::TokenOrder:
    return "TokenOrder";
  case SuffixOrderingProofKind::SourceAndTokenOrder:
    return "SourceAndTokenOrder";
  case SuffixOrderingProofKind::IncomparableMissingProducerFacts:
    return "IncomparableMissingProducerFacts";
  case SuffixOrderingProofKind::Unknown:
    return "Unknown";
  }
  llvm_unreachable("Invalid suffix ordering proof kind");
}

/// One suffix observer discovered by the observer graph.
///
/// `owner` identifies the later owner, `source`/`aTokens` give the comparable
/// position evidence, and `component` names the state component observed by
/// that owner.  The `observations` field preserves the full projected
/// observer summary so the proof model can make component-specific
/// repair/materialization decisions without rebuilding the graph.
struct SuffixStateObserverSite {
  uint64_t nodeId = 0;
  OwnerStateGraphNodeKind nodeKind = OwnerStateGraphNodeKind::Unknown;
  Owner owner;
  OwnerSourceRange source;
  OwnerTokenRange aTokens;
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  SuffixObservationKind observationKind = SuffixObservationKind::Unknown;
  OwnerObserverSummary observations;
  uint64_t order = 0;
  std::string componentKey;
  std::string detail;
};

/// Indexed observer result returned by the current proof model first-observer
/// queries.
struct SuffixObserverResult {
  uint64_t observerSiteIndex = 0;
  uint64_t nodeId = 0;
  Owner firstObserver;
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  SuffixObservationKind observationKind = SuffixObservationKind::Unknown;
  SuffixOrderingProofKind orderingProof = SuffixOrderingProofKind::Unknown;
  bool materializable = false;
  bool repairCanPrecede = false;
  SuffixStateObserverSite site;
};

/// Component-specific observer indexes for the persistent state graph.
///
/// The unkeyed vectors are the gateway surface; keyed maps preserve the
/// component identities so later proofs can ask more precise questions such
/// as "who observes macro FOO?" without scanning every owner.
struct OwnerStateGraphObserverIndex {
  std::vector<uint64_t> macroStateObservers;
  std::vector<uint64_t> definedOperatorObservers;
  std::vector<uint64_t> conditionalStateObservers;
  std::vector<uint64_t> lineStateObservers;
  std::vector<uint64_t> fileStateObservers;
  std::vector<uint64_t> fileNameStateObservers;
  std::vector<uint64_t> counterObservers;
  std::vector<uint64_t> pragmaStateObservers;
  std::vector<uint64_t> includeGuardStateObservers;
  std::vector<uint64_t> includeStateObservers;
  std::vector<uint64_t> unmodeledStateObservers;

  llvm::StringMap<std::vector<uint64_t>> observersByMacroName;
  llvm::StringMap<std::vector<uint64_t>> observersByCounterEvent;
  llvm::StringMap<std::vector<uint64_t>> observersByPragmaState;
  llvm::StringMap<std::vector<uint64_t>> observersByIncludeGuard;
  llvm::StringMap<std::vector<uint64_t>> observersByIncludeState;
  llvm::StringMap<std::vector<uint64_t>> observersByConditionalState;
};

/// graph census emitted to theorem/debug logs.
struct OwnerStateGraphAuditStats {
  uint64_t ownerNodes = 0;
  uint64_t zeroTokenStateNodes = 0;
  uint64_t observedStateComponents = 0;
  uint64_t mutatedStateComponents = 0;
  uint64_t incomparableNodes = 0;
  uint64_t missingProducerFacts = 0;
};


/// Persistent owner/state-event graph.
///
/// The graph is built once per RefoldEngine instance from producer facts.  It
/// is the canonical census used by suffix queries: callers should not perform
/// owner-specific scans for macro, include, line-control, pragma, conditional,
/// or counter events once this graph is available.
struct OwnerStateGraph {
  std::vector<OwnerStateGraphNode> nodes;
  std::vector<SuffixStateObserverSite> observerSites;
  OwnerStateGraphObserverIndex observerIndex;
  OwnerStateGraphAuditStats audit;

  bool Empty() const { return nodes.empty(); }
};

/// Immutable indexes over producer state facts.
///
/// Owner-state proofs ask the same questions many times while building suffix
/// observer graphs and checking edit-boundary transitions: "which producer
/// fact has this id?" and "which source-state facts are owned by this TU or
/// include instance?"  The model arrays are immutable after construction, so
/// we build these pointer indexes once per engine and keep every later query
/// linear only in the relevant owner-local bucket.
struct OwnerStateFactIndex {
  llvm::DenseMap<uint64_t, const RefoldModel::MacroDirective *>
      macroDirectiveById;
  llvm::DenseMap<uint64_t, const RefoldModel::LineControlEvent *>
      lineControlById;
  llvm::DenseMap<uint64_t, const RefoldModel::PragmaDirective *>
      pragmaById;
  llvm::DenseMap<uint64_t, const RefoldModel::MacroInvocation *>
      macroInvocationById;

  llvm::DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
      includesByParentIncludeKey;
  llvm::DenseMap<uint64_t, std::vector<const RefoldModel::MacroDirective *>>
      macroDirectivesByOwnerIncludeKey;
  llvm::DenseMap<uint64_t, std::vector<const RefoldModel::LineControlEvent *>>
      lineControlsByOwnerIncludeKey;
  llvm::DenseMap<uint64_t, std::vector<const RefoldModel::PragmaDirective *>>
      pragmasByOwnerIncludeKey;
  llvm::DenseMap<uint64_t, std::vector<const RefoldModel::CondGroup *>>
      condGroupsByParentIncludeKey;
  llvm::DenseMap<uint64_t, std::vector<const RefoldModel::MacroInvocation *>>
      macroInvocationsByOwnerIncludeKey;
};

/// Result of querying the preserved suffix after an edit boundary.
///
/// `sites` contains observers that are proven to occur after the boundary.
/// `hasIncomparableObserver` is a conservative signal: some owner observes the
/// requested component, but the available producer facts were insufficient to
/// order it relative to the boundary.  must treat that as an undischarged
/// suffix-stability obligation, not as absence of observation.
struct SuffixObserverQueryResult {
  std::vector<SuffixStateObserverSite> sites;
  std::vector<SuffixObserverResult> orderedObservers;
  std::vector<SuffixObserverResult> incomparableResults;
  std::optional<SuffixObserverResult> firstObserver = std::nullopt;
  bool hasIncomparableObserver = false;
  bool hasNoCanonicalSuffixOrder = false;
  OwnerObserverSummary incomparableObservers;

  bool Empty() const {
    return sites.empty() && !hasIncomparableObserver;
  }
};

// Owner-state proof behavior is implemented by RefoldOwnerStateProof.
// Use OwnerStateProof() instead of adding RefoldEngine forwarding wrappers.

/// How a source edit changes one state component at an edit boundary.
///
/// The *kind* of state transition is separate from the proof that makes that
/// transition safe.  A consumed #define, a replayed #line, and a literalized
/// __COUNTER__ occurrence may all affect suffix stability, but they have
/// different proof obligations and diagnostics.  The gateway below receives
/// one of these mutation kinds before it asks the suffix-observer graph
/// whether any preserved owner can observe the changed component.
enum class StateMutationKind : uint8_t {
  Consumed,
  Deleted,
  MovedEarlier,
  MovedLater,
  Replayed,
  PreservedAcrossReplacement,
  Materialized,
  Literalized,
  WidenedIntoClosure,
  Unknown
};

inline StringRef toString(StateMutationKind kind) {
  switch (kind) {
  case StateMutationKind::Consumed:
    return "Consumed";
  case StateMutationKind::Deleted:
    return "Deleted";
  case StateMutationKind::MovedEarlier:
    return "MovedEarlier";
  case StateMutationKind::MovedLater:
    return "MovedLater";
  case StateMutationKind::Replayed:
    return "Replayed";
  case StateMutationKind::PreservedAcrossReplacement:
    return "PreservedAcrossReplacement";
  case StateMutationKind::Materialized:
    return "Materialized";
  case StateMutationKind::Literalized:
    return "Literalized";
  case StateMutationKind::WidenedIntoClosure:
    return "WidenedIntoClosure";
  case StateMutationKind::Unknown:
    return "Unknown";
  }
  llvm_unreachable("Invalid state mutation kind");
}

/// Direct state-sensitive surface currently audited by the current proof
/// model.
///
/// This is an inventory vocabulary, not a selector.  Each entry names one
/// class of local source/preprocessor-state handling that must eventually be
/// represented by OwnerStateDelta, OwnerStateGraph, the state-transition
/// gateway, or a terminal proof failure before it can justify emitted bytes.
#define REFOLD_DIRECT_STATE_CHECK_KIND_LIST(REFOLD_X)                          \
REFOLD_X(Unknown)                                                            \
REFOLD_X(MacroDefinitionDirective)                                           \
REFOLD_X(MacroUndefDirective)                                                \
REFOLD_X(LineControlDirective)                                               \
REFOLD_X(BuiltinLineObserver)                                                \
REFOLD_X(BuiltinFileObserver)                                                \
REFOLD_X(BuiltinFileNameObserver)                                            \
REFOLD_X(CounterEvent)                                                       \
REFOLD_X(PragmaDirective)                                                    \
REFOLD_X(IncludeDirectiveState)                                              \
REFOLD_X(IncludeGuardState)                                                  \
REFOLD_X(ConditionalDirectiveState)                                          \
REFOLD_X(MacroExpansionState)                                                \
REFOLD_X(UnmodeledStateFact)

enum class DirectStateCheckKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_DIRECT_STATE_CHECK_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(DirectStateCheckKind kind) {
  switch (kind) {
#define REFOLD_X(name) case DirectStateCheckKind::name: return #name;
    REFOLD_DIRECT_STATE_CHECK_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}

#undef REFOLD_DIRECT_STATE_CHECK_KIND_LIST

/// How a direct state-sensitive local check is already closed.
///
/// This enum distinguishes legitimate local producer-fact collection from
/// legacy emission authority.  Only Unknown and ExplicitlyUnclosedLocalCheck
/// are audit findings; the other values document the proof surface that now
/// owns the state fact.
enum class DirectStateCheckClosureKind : uint8_t {
  Unknown,
  OwnerStateDeltaFact,
  OwnerStateGraphEdge,
  StateTransitionGatewayWitness,
  TerminalFallbackProofFailure,
  ExplicitlyUnclosedLocalCheck
};

inline StringRef toString(DirectStateCheckClosureKind kind) {
  switch (kind) {
  case DirectStateCheckClosureKind::Unknown:
    return "Unknown";
  case DirectStateCheckClosureKind::OwnerStateDeltaFact:
    return "OwnerStateDeltaFact";
  case DirectStateCheckClosureKind::OwnerStateGraphEdge:
    return "OwnerStateGraphEdge";
  case DirectStateCheckClosureKind::StateTransitionGatewayWitness:
    return "StateTransitionGatewayWitness";
  case DirectStateCheckClosureKind::TerminalFallbackProofFailure:
    return "TerminalFallbackProofFailure";
  case DirectStateCheckClosureKind::ExplicitlyUnclosedLocalCheck:
    return "ExplicitlyUnclosedLocalCheck";
  }
  llvm_unreachable("Invalid direct state check closure kind");
}

// Direct state-check inventory helpers live on RefoldOwnerStateProof.

/// Producer-closure proof for edits that would otherwise reverse-solve an
/// upstream directive from downstream B-side tokens.
///
/// Changing macro definitions, #line operands, include paths, or conditional
/// truth from the resulting expansion is forbidden unless the directive owner
/// itself belongs to the accepted owner closure.  Unknown closure proof is
/// also fail-closed because absence of a producer fact is not evidence that
/// reverse-solving is safe.
enum class DirectiveClosureStatus : uint8_t {
  NotADirectiveStateRewrite,
  DirectiveOwnerInsideAcceptedClosure,
  DirectiveOwnerOutsideAcceptedClosure,
  Unknown
};

inline StringRef toString(DirectiveClosureStatus status) {
  switch (status) {
  case DirectiveClosureStatus::NotADirectiveStateRewrite:
    return "NotADirectiveStateRewrite";
  case DirectiveClosureStatus::DirectiveOwnerInsideAcceptedClosure:
    return "DirectiveOwnerInsideAcceptedClosure";
  case DirectiveClosureStatus::DirectiveOwnerOutsideAcceptedClosure:
    return "DirectiveOwnerOutsideAcceptedClosure";
  case DirectiveClosureStatus::Unknown:
    return "Unknown";
  }
  llvm_unreachable("Invalid directive closure status");
}

/// Typed witness that the preserved suffix does not observe the changed
/// state.
struct SuffixUnobservedWitness {
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  OwnerStateBoundary boundary;
  std::string detail;
};

/// Typed witness that equivalent state is repaired before the first observer.
struct StateRepairWitness {
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  OwnerStateBoundary boundary;
  std::optional<SuffixObserverResult> firstObserver = std::nullopt;
  std::string detail;
};

/// Typed witness that a suffix observer is materialized and no longer
/// observes the changed preprocessor state through preserved source spelling.
struct OwnerMaterializationWitness {
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  OwnerStateBoundary boundary;
  std::optional<SuffixObserverResult> materializedObserver = std::nullopt;
  std::string detail;
};

/// Typed witness that the edit closure was widened to include every relevant
/// suffix observer before it could observe the changed state.
struct ClosureWideningWitness {
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  OwnerStateBoundary boundary;
  std::optional<SuffixObserverResult> widenedThroughObserver = std::nullopt;
  std::string detail;
};

/// Typed witness that a stateful builtin event was replaced by a literal
/// whose value was derived from the assigned B-side envelope.
struct LiteralizationWitness {
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  OwnerStateBoundary boundary;
  std::optional<SuffixObserverResult> literalizedObserver = std::nullopt;
  std::string detail;
};

/// Typed witness for the fail-closed terminal state-stability case.
struct TerminalStateFailureWitness {
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  OwnerStateBoundary boundary;
  TerminalFallbackProofFailure failure;
  bool hasFailure = false;
  std::string detail;
};

/// Compact tagged wrapper for the current proof model suffix-stability
/// witnesses.
///
/// The individual witness structs above are the theorem-facing payloads. This
/// wrapper exists only to pass one typed witness through the existing C++17
/// code without introducing a second hierarchy or duplicated switch logic.
enum class SuffixStabilityWitnessKind : uint8_t {
  None,
  SuffixUnobserved,
  StateRepair,
  OwnerMaterialization,
  ClosureWidening,
  Literalization,
  TerminalStateFailure
};

inline StringRef toString(SuffixStabilityWitnessKind kind) {
  switch (kind) {
  case SuffixStabilityWitnessKind::None:
    return "None";
  case SuffixStabilityWitnessKind::SuffixUnobserved:
    return "SuffixUnobserved";
  case SuffixStabilityWitnessKind::StateRepair:
    return "StateRepair";
  case SuffixStabilityWitnessKind::OwnerMaterialization:
    return "OwnerMaterialization";
  case SuffixStabilityWitnessKind::ClosureWidening:
    return "ClosureWidening";
  case SuffixStabilityWitnessKind::Literalization:
    return "Literalization";
  case SuffixStabilityWitnessKind::TerminalStateFailure:
    return "TerminalStateFailure";
  }
  llvm_unreachable("Invalid suffix-stability witness kind");
}

struct SuffixStabilityWitness {
  SuffixStabilityWitnessKind kind = SuffixStabilityWitnessKind::None;
  std::optional<SuffixUnobservedWitness> suffixUnobserved = std::nullopt;
  std::optional<StateRepairWitness> stateRepair = std::nullopt;
  std::optional<OwnerMaterializationWitness> ownerMaterialization =
      std::nullopt;
  std::optional<ClosureWideningWitness> closureWidening = std::nullopt;
  std::optional<LiteralizationWitness> literalization = std::nullopt;
  std::optional<TerminalStateFailureWitness> terminalFailure = std::nullopt;

  static SuffixStabilityWitness None() { return SuffixStabilityWitness(); }

  static SuffixStabilityWitness From(SuffixUnobservedWitness witness) {
    SuffixStabilityWitness out;
    out.kind = SuffixStabilityWitnessKind::SuffixUnobserved;
    out.suffixUnobserved = std::move(witness);
    return out;
  }

  static SuffixStabilityWitness From(StateRepairWitness witness) {
    SuffixStabilityWitness out;
    out.kind = SuffixStabilityWitnessKind::StateRepair;
    out.stateRepair = std::move(witness);
    return out;
  }

  static SuffixStabilityWitness From(OwnerMaterializationWitness witness) {
    SuffixStabilityWitness out;
    out.kind = SuffixStabilityWitnessKind::OwnerMaterialization;
    out.ownerMaterialization = std::move(witness);
    return out;
  }

  static SuffixStabilityWitness From(ClosureWideningWitness witness) {
    SuffixStabilityWitness out;
    out.kind = SuffixStabilityWitnessKind::ClosureWidening;
    out.closureWidening = std::move(witness);
    return out;
  }

  static SuffixStabilityWitness From(LiteralizationWitness witness) {
    SuffixStabilityWitness out;
    out.kind = SuffixStabilityWitnessKind::Literalization;
    out.literalization = std::move(witness);
    return out;
  }

  static SuffixStabilityWitness From(TerminalStateFailureWitness witness) {
    SuffixStabilityWitness out;
    out.kind = SuffixStabilityWitnessKind::TerminalStateFailure;
    out.terminalFailure = std::move(witness);
    return out;
  }
};

/// Canonical theorem-facing result produced by the state gateway.
///
/// The gateway returns this proof directly so callers cannot approve state
/// transitions by reading component-local booleans beside the typed theorem
/// witness.  The carrier records the before/after state deltas supplied by the
/// caller, the typed suffix witnesses accepted by the gateway, the closure-
/// widening subset needed by theorem consumers, and the classified terminal
/// failure when the transition is outside the strict domain. Component-local
/// approvals should
/// move onto this proof instead of introducing another parallel state flag.
struct StateTransitionProof {
  OwnerStateDelta before;
  OwnerStateDelta after;
  std::vector<SuffixStabilityWitness> suffixWitnesses;
  std::vector<ClosureWideningWitness> wideningWitnesses;
  std::optional<TerminalFallbackProofFailure> failure = std::nullopt;
};

/// Request passed through the single state-transition gateway.
struct StateTransitionGatewayRequest {
  OwnerStateBoundary boundary;
  OwnerStateComponent component = OwnerStateComponent::Unknown;
  StateMutationKind mutation = StateMutationKind::Unknown;
  SuffixStabilityWitness witness;
  OwnerStateDelta before;
  OwnerStateDelta after;
  std::string stage;
  std::string detail;
  bool requireKnownObserver = false;

  /// reverse-solving gate.  When true, the request represents a state
  /// rewrite whose source directive would have to be inferred from downstream
  /// B-side expansion unless the directive owner is already inside the
  /// accepted edit closure.  The gateway rejects Outside/Unknown before
  /// consulting suffix observers, because suffix repair cannot justify
  /// changing unedited upstream source state.
  bool requiresDirectiveClosureProof = false;
  DirectiveClosureStatus directiveClosureStatus =
      DirectiveClosureStatus::NotADirectiveStateRewrite;
  std::string directiveKind;
};


} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATETYPES_H

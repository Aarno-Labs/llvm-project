//===--- RefoldOwnerStateProof.h -------------------------------*- C++ -*-===//
//
// Part of the clang-refold proof system.
//
// This header marks the owner-state proof extraction boundary.  The
// owner-state vocabulary lives in RefoldOwnerStateTypes.h while the transition
// graph, suffix-observer queries, and state-gateway caches are owned by
// RefoldOwnerStateProof.cpp.  Keeping carrier records separate from the mutable
// proof service shrinks RefoldEngine.h without changing proof policy.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldTerminalProofSink.h"
#include "source/RefoldToken.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "util/RefoldPathIdentity.h"

#include "clang/Basic/LangOptions.h"

namespace clang {
namespace refold {


/// Immutable construction inputs borrowed by the owner-state proof service.
///
/// Keeping these facts in one small aggregate makes the service's source, token,
/// path, and proof dependencies explicit.  The service does not mutate these
/// buffers.
struct RefoldOwnerStateProofInputs {
  const RefoldModel &model;
  llvm::ArrayRef<PPTok> aToks;
  llvm::ArrayRef<PPTok> bToks;
  const clang::LangOptions &lexLang;
};

class RefoldTheoremAudit;

class RefoldOwnerStateProof {
  // Owner-state carriers are namespace-level value types from
  // RefoldOwnerStateTypes.h.  Keep these short aliases local to the service so
  // the implementation remains readable without pulling the definitions back
  // into RefoldEngine.
  using OwnerKind = ::clang::refold::OwnerKind;
  using Owner = ::clang::refold::Owner;
  using OwnerSourceRange = ::clang::refold::OwnerSourceRange;
  using OwnerTokenRange = ::clang::refold::OwnerTokenRange;
  using OwnerObserverSummary = ::clang::refold::OwnerObserverSummary;
  using MacroStateIdentity = ::clang::refold::MacroStateIdentity;
  using MacroObservationKind = ::clang::refold::MacroObservationKind;
  using MacroStateObservation = ::clang::refold::MacroStateObservation;
  using LineDirectiveOperandProvenance = ::clang::refold::LineDirectiveOperandProvenance;
  using LineControlStateIdentity = ::clang::refold::LineControlStateIdentity;
  using BuiltinLocationObservationKind = ::clang::refold::BuiltinLocationObservationKind;
  using BuiltinLocationObservation = ::clang::refold::BuiltinLocationObservation;
  using CounterEventIdentity = ::clang::refold::CounterEventIdentity;
  using IncludeStateIdentity = ::clang::refold::IncludeStateIdentity;
  using IncludeGuardObservationKind = ::clang::refold::IncludeGuardObservationKind;
  using IncludeGuardStateIdentity = ::clang::refold::IncludeGuardStateIdentity;
  using PragmaStateClassification = ::clang::refold::PragmaStateClassification;
  using PragmaStateIdentity = ::clang::refold::PragmaStateIdentity;
  using ConditionalStateRole = ::clang::refold::ConditionalStateRole;
  using ConditionalStateIdentity = ::clang::refold::ConditionalStateIdentity;
  using MissingStateFactKind = ::clang::refold::MissingStateFactKind;
  using MissingStateFact = ::clang::refold::MissingStateFact;
  using OwnerStateFacts = ::clang::refold::OwnerStateFacts;
  using StateRequirements = ::clang::refold::StateRequirements;
  using StateObservations = ::clang::refold::StateObservations;
  using StateMutations = ::clang::refold::StateMutations;
  using StateGuarantees = ::clang::refold::StateGuarantees;
  using OwnerStateDelta = ::clang::refold::OwnerStateDelta;
  using OwnerClosure = ::clang::refold::OwnerClosure;
  using OwnerStateComponent = ::clang::refold::OwnerStateComponent;
  using OwnerStateBoundary = ::clang::refold::OwnerStateBoundary;
  using OwnerStateGraphNodeKind = ::clang::refold::OwnerStateGraphNodeKind;
  using OwnerStateGraphNode = ::clang::refold::OwnerStateGraphNode;
  using SuffixObservationKind = ::clang::refold::SuffixObservationKind;
  using SuffixOrderingProofKind = ::clang::refold::SuffixOrderingProofKind;
  using SuffixStateObserverSite = ::clang::refold::SuffixStateObserverSite;
  using SuffixObserverResult = ::clang::refold::SuffixObserverResult;
  using OwnerStateGraphObserverIndex = ::clang::refold::OwnerStateGraphObserverIndex;
  using OwnerStateGraphAuditStats = ::clang::refold::OwnerStateGraphAuditStats;
  using OwnerStateGraph = ::clang::refold::OwnerStateGraph;
  using OwnerStateFactIndex = ::clang::refold::OwnerStateFactIndex;
  using SuffixObserverQueryResult = ::clang::refold::SuffixObserverQueryResult;
  using StateMutationKind = ::clang::refold::StateMutationKind;
  using DirectStateCheckKind = ::clang::refold::DirectStateCheckKind;
  using DirectStateCheckClosureKind = ::clang::refold::DirectStateCheckClosureKind;
  using DirectiveClosureStatus = ::clang::refold::DirectiveClosureStatus;
  using SuffixUnobservedWitness = ::clang::refold::SuffixUnobservedWitness;
  using StateRepairWitness = ::clang::refold::StateRepairWitness;
  using OwnerMaterializationWitness = ::clang::refold::OwnerMaterializationWitness;
  using ClosureWideningWitness = ::clang::refold::ClosureWideningWitness;
  using LiteralizationWitness = ::clang::refold::LiteralizationWitness;
  using TerminalStateFailureWitness = ::clang::refold::TerminalStateFailureWitness;
  using SuffixStabilityWitnessKind = ::clang::refold::SuffixStabilityWitnessKind;
  using SuffixStabilityWitness = ::clang::refold::SuffixStabilityWitness;
  using StateTransitionProof = ::clang::refold::StateTransitionProof;
  using StateTransitionGatewayRequest = ::clang::refold::StateTransitionGatewayRequest;
  using TerminalFallbackProofFailure = ::clang::refold::TerminalFallbackProofFailure;

public:
  explicit RefoldOwnerStateProof(RefoldOwnerStateProofInputs inputs,
                                const RefoldPathIdentity &paths,
                                const RefoldTokenTextAnalysis &tokenText,
                                const RefoldMacroTopology &macroTopology,
                                const RefoldTheoremAudit &theoremAudit,
                                const RefoldTerminalProofSink &terminalSink);

  OwnerStateFactIndex BuildOwnerStateFactIndex() const;
  const OwnerStateFactIndex &GetOwnerStateFactIndex() const;

  static bool IsVirtualInitialMacroDirectiveSource(StringRef sitePath);
  static uint64_t OwnerIncludeBucketKey(std::optional<uint64_t> includeId);
  static std::optional<uint64_t> OwnerSourceBucketKey(const Owner &owner);
  static std::string OwnerStateDeltaCacheKey(const Owner &owner);

  OwnerStateDelta BuildOwnerStateDelta(const Owner &owner) const;
  OwnerStateDelta GetOwnerStateDelta(const Owner &owner) const;

  static OwnerStateDelta BuildTheoremStateDelta(
      const OwnerStateFacts &facts, const OwnerStateDelta &directDelta);
  static bool OwnerStateDeltaHasUnmodeledState(const OwnerStateDelta &delta);
  static bool OwnerStateDeltaHasUnknownPragmaState(
      const OwnerStateDelta &delta);
  static bool OwnerStateDeltaMutatesAnyState(const OwnerStateDelta &delta);
  static OwnerObserverSummary
  OwnerStateDeltaToObserverSummary(const OwnerStateDelta &delta);

  OwnerClosure AttachCanonicalStateSummary(OwnerClosure closure) const;
  OwnerStateGraph BuildOwnerStateGraph() const;
  const OwnerStateGraph &GetOwnerStateGraph() const;
  SuffixObserverQueryResult
  FindSuffixObservers(const OwnerStateBoundary &boundary,
                      OwnerStateComponent component) const;

  void AuditDirectStateCheckClosure(DirectStateCheckKind checkKind,
                                    OwnerStateComponent component,
                                    DirectStateCheckClosureKind closure,
                                    StateMutationKind mutation,
                                    llvm::StringRef stage,
                                    llvm::StringRef detail) const;
  static DirectStateCheckKind
  DirectStateCheckKindForComponent(OwnerStateComponent component);
  static DirectStateCheckKind
  DirectStateCheckKindForGraphNode(OwnerStateGraphNodeKind kind);
  static OwnerStateComponent
  DirectStateComponentForGraphNode(OwnerStateGraphNodeKind kind);

  static std::vector<OwnerStateComponent>
  StateComponentsMutatedByDelta(const OwnerStateDelta &delta);
  static OwnerStateComponent
  StateComponentForMissingStateFact(MissingStateFactKind kind);
  static TerminalFallbackProofFailure
  MissingStateFactTerminalFailure(MissingStateFactKind kind, StringRef detail);
  static TerminalFallbackProofFailure
  SuffixStabilityTerminalFailureForComponent(OwnerStateComponent component);
  static TerminalFallbackProofFailure ReverseSolvedDirectiveTerminalFailure(
      OwnerStateComponent component, const OwnerStateBoundary &boundary,
      llvm::StringRef directiveKind, llvm::StringRef detail);
  static SuffixStabilityWitness BuildReverseSolvedDirectiveTerminalWitness(
      const OwnerStateBoundary &boundary, OwnerStateComponent component,
      llvm::StringRef directiveKind, llvm::StringRef detail);

  StateTransitionProof CheckReverseSolvedDirectiveAcrossEditBoundary(
      const OwnerStateBoundary &boundary, OwnerStateComponent component,
      StateMutationKind mutation, DirectiveClosureStatus directiveClosureStatus,
      llvm::StringRef directiveKind, llvm::StringRef stage,
      llvm::StringRef detail) const;
  static OwnerStateComponent
  ComponentNamedBySuffixStabilityWitness(const SuffixStabilityWitness &witness);
  static bool SuffixStabilityWitnessNamesComponent(
      const SuffixStabilityWitness &witness, OwnerStateComponent component);
  StateTransitionProof CheckStateTransitionAcrossEditBoundary(
      const StateTransitionGatewayRequest &request) const;
  StateTransitionProof CheckStateTransitionAcrossEditBoundary(
      const OwnerStateBoundary &boundary, OwnerStateComponent component,
      StateMutationKind mutation, SuffixStabilityWitness witness,
      llvm::StringRef stage, llvm::StringRef detail,
      bool requireKnownObserver) const;
  static SuffixStabilityWitness BuildStateTransitionWitness(
      SuffixStabilityWitnessKind kind, OwnerStateComponent component,
      const OwnerStateBoundary &boundary, llvm::StringRef detail);

  static OwnerStateBoundary
  CounterStateBoundaryForEvent(const CounterEventIdentity &event);
  static std::string
  FormatCounterEventForWitness(const CounterEventIdentity &event);
  static bool IsLineControlStateComponent(OwnerStateComponent component);
  static std::string
  FormatLineControlEventForWitness(const LineControlStateIdentity &event);
  static std::string FormatBuiltinLocationObservationForWitness(
      const BuiltinLocationObservation &observation);
  static OwnerStateBoundary
  IncludeStateBoundaryForIncludeSite(const RefoldModel::IncludeItem &include);

  static SuffixObservationKind
  ObservationKindForComponent(OwnerStateComponent component);
  static ArrayRef<uint64_t> ObserverSiteIndexesForComponent(
      const OwnerStateGraph &graph, OwnerStateComponent component);
  static bool OwnerObserverSummaryObservesComponent(
      const OwnerObserverSummary &summary, OwnerStateComponent component);

  bool OwnerMatchesSourceSite(const Owner &owner, StringRef file,
                              std::optional<uint64_t> ownerIncludeId,
                              uint64_t begin, uint64_t end) const;

private:
  const RefoldModel &model_;
  llvm::ArrayRef<PPTok> aToks_;
  const clang::LangOptions &lexLang_;
  const RefoldPathIdentity &paths_;
  const RefoldTokenTextAnalysis &tokenText_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldTheoremAudit &theoremAudit_;
  const RefoldTerminalProofSink &terminalSink_;

  /// Lazily built owner-state census indexes.  Moving these caches with the
  /// proof service keeps owner-state state out of RefoldEngine instead of
  /// leaving a data-only remnant behind after method extraction.
  mutable std::optional<OwnerStateFactIndex> ownerStateFactIndexCache_;
  mutable std::optional<OwnerStateGraph> ownerStateGraphCache_;
  mutable llvm::StringMap<OwnerStateDelta> ownerStateDeltaCache_;
};

/// Diagnostic pragma state action admitted by the owner-state proof.
///
/// The parser intentionally recognizes only the deterministic diagnostic
/// push/pop/setting sublanguage whose stack effect can be modeled exactly.
enum class DiagnosticPragmaStateAction { Push, Pop, Setting };

/// Parsed spelling for a diagnostic pragma-state directive.
///
/// StringRef fields point into the caller-provided source slice.  Callers must
/// not retain this object beyond the lifetime of that slice.
struct ParsedDiagnosticPragmaStateDirective {
  StringRef namespaceName;
  StringRef actionName;
  StringRef optionSpelling;
  DiagnosticPragmaStateAction action = DiagnosticPragmaStateAction::Setting;
};

/// Return true iff a retained raw-lexer comment token has complete spelling.
///
/// This is shared with the expansion fallback helpers because they are textually
/// included into RefoldEngine.cpp and still need the same fail-closed raw-comment
/// completeness predicate after owner-state proof extraction.
bool rawLexerCommentTokenIsComplete(StringRef spelling);

/// Return true iff `text` is whitespace plus complete C/C++ comments.
///
/// This is the shared raw-lexer trivia theorem used by both owner-state proof
/// extraction and the remaining engine-side mixed-owner tiling code.
bool sourceTextIsOnlyWhitespaceAndCompleteComments(StringRef text,
                                                   const LangOptions &lang);

/// Parse the strict diagnostic pragma-state sublanguage modeled by refold.
///
/// Unknown pragmas, malformed diagnostic directives, and directives with
/// non-trivia suffix bytes are rejected fail-closed.
std::optional<ParsedDiagnosticPragmaStateDirective>
parseDiagnosticPragmaStateDirective(StringRef text, const LangOptions &lang);

/// Return true iff two non-empty half-open intervals overlap.
bool intervalsOverlap(uint64_t beginA, uint64_t endA, uint64_t beginB,
                      uint64_t endB);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H

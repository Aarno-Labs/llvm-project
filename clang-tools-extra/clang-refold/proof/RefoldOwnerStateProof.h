//===--- RefoldOwnerStateProof.h -------------------------------*- C++ -*-===//
//
// Owner-state proof service for clang-refold.
//
// RefoldOwnerStateProof owns the mutable proof-side indexes, transition-graph
// cache, suffix-observer queries, and uniform state-transition gateway.
// Owner-state carrier records live in RefoldOwnerStateTypes.h; this service
// consumes those carriers together with model/source facts and theorem-audit
// hooks to prove whether an emitted owner realization preserves preprocessing
// state or must fail closed through a typed terminal obligation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldToken.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "util/RefoldPathIdentity.h"

#include "clang/Basic/LangOptions.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>

namespace clang {
namespace refold {

/// Immutable construction inputs borrowed by the owner-state proof service.
///
/// Keeping these facts in one small aggregate makes the service's source,
/// token, path, and proof dependencies explicit.  The service does not mutate
/// these buffers.
/// One run's owner-state graph, recorded the first time it is built.
///
/// `BuildOwnerStateGraph` reads the producer model and the A token stream and
/// nothing else.  Both are run constants -- an attempt narrows which owners
/// must expand and which anchors it plans from, and a candidate simulation is
/// handed a different alignment, but neither rewrites the producer facts or the
/// A stream -- so every attempt of one run, every candidate simulation it
/// enumerates, and the resolution probe that stands in for the next attempt
/// census the same owners and reach an identical graph.
///
/// Re-deriving it is not cheap.  The build attaches a canonical state summary
/// to every owner the producer recorded, so its cost grows with the owner
/// count rather than with the edit; on a translation unit whose ambiguous
/// windows enumerate many candidate maps it is the largest single cost in a
/// pass.  Recording it lets a run pay that once however many candidates it
/// simulates.
///
/// This is a memo, not a budget.  It never coarsens the census, never bounds
/// it, and never changes which owners are graphed: a recorded graph is returned
/// only to a caller presenting the exact producer document and A stream it was
/// built from.
struct OwnerStateGraphMemo {
  /// Whether `graph` holds a result this run already built.
  bool recorded = false;

  /// Identity of the producer facts `graph` was built from.
  ///
  /// Every model field is a deterministic function of the producer document,
  /// and a read-only clone shares that document rather than reparsing it, so
  /// agreeing on the document and the map version is agreeing on the facts the
  /// census reads.  See `RefoldModel::GetProducerDocument`.
  const llvm::json::Object *producerDocument = nullptr;
  llvm::StringRef producerVersion;

  /// Identity of the A token stream the census was taken over.
  const PPTok *aTokensData = nullptr;
  size_t aTokensSize = 0;

  /// The recorded graph, copied out on every match.
  OwnerStateGraph graph;

  /// Return whether a recorded graph was built from exactly these inputs.
  bool MatchesInputs(const RefoldModel &model,
                     llvm::ArrayRef<PPTok> aToks) const {
    return recorded && producerDocument == model.GetProducerDocument() &&
           producerVersion == model.GetVersion() &&
           aTokensData == aToks.data() && aTokensSize == aToks.size();
  }

  /// Record \p built as this run's owner-state graph for these inputs.
  void Record(const RefoldModel &model, llvm::ArrayRef<PPTok> aToks,
              OwnerStateGraph built) {
    producerDocument = model.GetProducerDocument();
    producerVersion = model.GetVersion();
    aTokensData = aToks.data();
    aTokensSize = aToks.size();
    graph = std::move(built);
    recorded = true;
  }
};

/// Immutable construction inputs borrowed by the owner-state proof service.
///
/// Keeping these facts in one small aggregate makes the service's source,
/// token, path, and proof dependencies explicit.  The service does not mutate
/// these buffers.
struct RefoldOwnerStateProofInputs {
  const RefoldModel &model;
  llvm::ArrayRef<PPTok> aToks;
  llvm::ArrayRef<PPTok> bToks;
  const clang::LangOptions &lexLang;

  /// This run's recorded owner-state graph, or null to always build one.
  ///
  /// Null for any engine built outside the narrowing loop that owns the memo.
  /// See `OwnerStateGraphMemo`.
  OwnerStateGraphMemo *ownerStateGraphMemo = nullptr;
};

class RefoldTheoremAudit;

/// Builds and checks owner-state proof facts for source-preserving edits.
///
/// The service owns owner-state fact indexing, delta/graph construction,
/// suffix-stability checks, direct-state gateway auditing, and typed terminal
/// failures for state facts that cannot be proven.
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
  using LineDirectiveOperandProvenance =
      ::clang::refold::LineDirectiveOperandProvenance;
  using LineControlStateIdentity = ::clang::refold::LineControlStateIdentity;
  using BuiltinLocationObservationKind =
      ::clang::refold::BuiltinLocationObservationKind;
  using BuiltinLocationObservation =
      ::clang::refold::BuiltinLocationObservation;
  using CounterEventIdentity = ::clang::refold::CounterEventIdentity;
  using IncludeStateIdentity = ::clang::refold::IncludeStateIdentity;
  using IncludeGuardObservationKind =
      ::clang::refold::IncludeGuardObservationKind;
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
  using OwnerStateGraphObserverIndex =
      ::clang::refold::OwnerStateGraphObserverIndex;
  using OwnerStateGraphAuditStats = ::clang::refold::OwnerStateGraphAuditStats;
  using OwnerStateGraph = ::clang::refold::OwnerStateGraph;
  using OwnerStateFactIndex = ::clang::refold::OwnerStateFactIndex;
  using SuffixObserverQueryResult = ::clang::refold::SuffixObserverQueryResult;
  using StateMutationKind = ::clang::refold::StateMutationKind;
  using DirectStateCheckKind = ::clang::refold::DirectStateCheckKind;
  using DirectStateCheckClosureKind =
      ::clang::refold::DirectStateCheckClosureKind;
  using SuffixUnobservedWitness = ::clang::refold::SuffixUnobservedWitness;
  using StateRepairWitness = ::clang::refold::StateRepairWitness;
  using OwnerMaterializationWitness =
      ::clang::refold::OwnerMaterializationWitness;
  using ClosureWideningWitness = ::clang::refold::ClosureWideningWitness;
  using LiteralizationWitness = ::clang::refold::LiteralizationWitness;
  using TerminalStateFailureWitness =
      ::clang::refold::TerminalStateFailureWitness;
  using SuffixStabilityWitnessKind =
      ::clang::refold::SuffixStabilityWitnessKind;
  using SuffixStabilityWitness = ::clang::refold::SuffixStabilityWitness;
  using StateTransitionProof = ::clang::refold::StateTransitionProof;
  using StateTransitionGatewayRequest =
      ::clang::refold::StateTransitionGatewayRequest;
  using TerminalFallbackProofFailure =
      ::clang::refold::TerminalFallbackProofFailure;

public:
  explicit RefoldOwnerStateProof(RefoldOwnerStateProofInputs inputs,
                                 const RefoldPathIdentity &paths,
                                 const RefoldTokenTextAnalysis &tokenText,
                                 const RefoldMacroTopology &macroTopology,
                                 const RefoldTheoremAudit &theoremAudit,
                                 const RefoldTerminalProofSink &terminalSink);

  /// Build the immutable state-fact index used by owner-state delta queries.
  OwnerStateFactIndex BuildOwnerStateFactIndex() const;
  /// Return the lazily built state-fact index.
  const OwnerStateFactIndex &GetOwnerStateFactIndex() const;

  /// Return true for virtual macro-definition streams that seed the initial
  /// preprocessor environment but are not source-editable suffix graph events.
  static bool IsVirtualInitialMacroDirectiveSource(StringRef sitePath);
  /// DenseMap bucket key for an optional include-owner id.
  static uint64_t OwnerIncludeBucketKey(std::optional<uint64_t> includeId);
  /// Source-owner bucket used for TU/include-local fact scans.
  static std::optional<uint64_t> OwnerSourceBucketKey(const Owner &owner);
  /// Stable, allocation-free cache key for canonical owner-state deltas.
  ///
  /// Every field is the corresponding `Owner` field with `std::nullopt`
  /// projected onto the reserved sentinel `kNoId`.  Two owners share a key
  /// exactly when all eight identity fields agree, which is the same identity
  /// the previous formatted string key expressed.
  struct OwnerStateDeltaCacheKey {
    static constexpr uint64_t kNoId = std::numeric_limits<uint64_t>::max();

    unsigned kind = 0;
    uint64_t includeId = kNoId;
    uint64_t macroInvocationId = kNoId;
    uint64_t macroDirectiveId = kNoId;
    uint64_t lineControlId = kNoId;
    uint64_t pragmaId = kNoId;
    uint64_t condGroupId = kNoId;
    uint64_t condArmId = kNoId;

    /// Total order over the eight identity fields, in the same field order the
    /// previous formatted key concatenated them.
    bool operator<(const OwnerStateDeltaCacheKey &other) const {
      return std::tie(kind, includeId, macroInvocationId, macroDirectiveId,
                      lineControlId, pragmaId, condGroupId, condArmId) <
             std::tie(other.kind, other.includeId, other.macroInvocationId,
                      other.macroDirectiveId, other.lineControlId,
                      other.pragmaId, other.condGroupId, other.condArmId);
    }
  };

  /// Project an owner onto its canonical delta cache key.
  static OwnerStateDeltaCacheKey MakeOwnerStateDeltaCacheKey(const Owner &owner);

  /// Build the conservative state summary for a concrete owner.
  ///
  /// The result is monotone: missing producer facts or unknown owner identity
  /// set a MissingStateFact instead of clearing obligations.  This helper is a
  /// theorem-facing census primitive only; later proof passes decide how to
  /// consume or enforce the returned obligations.
  OwnerStateDelta BuildOwnerStateDelta(const Owner &owner) const;
  /// Cached wrapper around BuildOwnerStateDelta for non-audit runs.
  OwnerStateDelta GetOwnerStateDelta(const Owner &owner) const;

  /// Project producer-derived builder facts into the theorem-facing delta.
  ///
  /// Project precise component facts and explicit MissingStateFact markers
  /// into Entry/Observes/Mutates/Exit, then merge already-typed delta facts.
  static OwnerStateDelta
  BuildTheoremStateDelta(const OwnerStateFacts &facts,
                         const OwnerStateDelta &directDelta);
  /// Query helper for component-specific missing producer facts in a state
  /// delta.
  static bool OwnerStateDeltaHasUnmodeledState(const OwnerStateDelta &delta);
  /// Query helper for pragma-state uncertainty in a state delta.
  static bool
  OwnerStateDeltaHasUnknownPragmaState(const OwnerStateDelta &delta);
  /// Query helper for whether a canonical state delta mutates any tracked
  /// preprocessor state component.
  static bool OwnerStateDeltaMutatesAnyState(const OwnerStateDelta &delta);
  /// Convert the observations in a state delta into the compact observer
  /// summary used by suffix-observer graph queries.
  static OwnerObserverSummary
  OwnerStateDeltaToObserverSummary(const OwnerStateDelta &delta);

  /// Return `closure` with its canonical owner-state facts attached.
  ///
  /// Existing callers may still construct passive closures without summaries.
  /// This helper gives new proof code one normalization point that annotates a
  /// closure from producer metadata before composing it with neighboring
  /// owners.
  OwnerClosure AttachCanonicalStateSummary(OwnerClosure closure) const;
  /// Build the deterministic persistent owner/state-event graph.
  ///
  /// The graph contains ordinary token owners, include entry/exit events,
  /// nested macro expansion owners, and all zero-token state directives
  /// recorded by the producer.  It is a census only: suffix-stability
  /// enforcement remains a responsibility.
  OwnerStateGraph BuildOwnerStateGraph() const;
  /// Return the cached owner/state-event graph, building it once.
  const OwnerStateGraph &GetOwnerStateGraph() const;
  /// Return later suffix observers of `component` after `boundary`.
  ///
  /// This is the canonical suffix-observer API.  Callers that need raw graph
  /// nodes should consume `GetOwnerStateGraph()` directly rather than rebuild a
  /// compatibility projection; this keeps ordering and observer indexing in one
  /// proof surface.
  ///
  /// The query is conservative.  If an observer exists but cannot be ordered
  /// against the boundary using source or token facts, the result marks it as
  /// incomparable so the caller cannot accidentally discharge the obligation.
  SuffixObserverQueryResult
  FindSuffixObservers(const OwnerStateBoundary &boundary,
                      OwnerStateComponent component) const;

  /// Use \p memo as this run's recorded owner-state graph.
  ///
  /// The service graph is built before the narrowing loop that owns the memo
  /// can hand it out, and no census is taken during construction, so the memo
  /// is attached afterwards.  Attaching one after a graph has already been
  /// built for this engine is refused: the engine would then answer from its
  /// own census while recording nothing, which hides the memo from every later
  /// consumer.
  void SetOwnerStateGraphMemo(OwnerStateGraphMemo *memo) {
    assert(!ownerStateGraphCache_ &&
           "owner-state graph memo attached after this engine built a census");
    ownerStateGraphMemo_ = memo;
  }

  /// Replay the direct-state-check inventory a graph build would have recorded.
  ///
  /// Building the census records one inventory item per graph node as it is
  /// created.  A run that replays a recorded graph never runs that build, so
  /// this walks the recorded nodes and records the identical items.  The
  /// inventory is a per-engine no-legacy audit surface whose enablement differs
  /// between an attempt and a candidate simulation, so it is replayed for each
  /// consumer rather than recorded alongside the graph.  Every item it emits is
  /// a closed `OwnerStateGraphEdge`, which is counted and never reported as a
  /// finding, so replay order does not matter.
  void ReplayOwnerStateGraphAudit(const OwnerStateGraph &graph) const;

  /// Record one direct-state-check inventory item for no-legacy audit.
  void AuditDirectStateCheckClosure(DirectStateCheckKind checkKind,
                                    OwnerStateComponent component,
                                    DirectStateCheckClosureKind closure,
                                    StateMutationKind mutation,
                                    llvm::StringRef stage,
                                    llvm::StringRef detail) const;
  /// Map a state component to the closest direct-state-check inventory key.
  static DirectStateCheckKind
  DirectStateCheckKindForComponent(OwnerStateComponent component);
  /// Map owner-state graph nodes back to the direct-check inventory.
  static DirectStateCheckKind
  DirectStateCheckKindForGraphNode(OwnerStateGraphNodeKind kind);
  /// Map owner-state graph nodes back to their theorem-facing state component.
  static OwnerStateComponent
  DirectStateComponentForGraphNode(OwnerStateGraphNodeKind kind);

  /// Return the theorem-facing state components mutated by `delta`.
  ///
  /// Callers that consume or move an entire owner enumerate mutated components
  /// directly from the canonical OwnerStateDelta.
  static std::vector<OwnerStateComponent>
  StateComponentsMutatedByDelta(const OwnerStateDelta &delta);
  /// Return the theorem state component made uncertain by one
  /// component-specific missing producer fact.  This converts missing-fact
  /// markers into precise suffix-stability obligations instead of one global
  /// unmodeled state surface.
  static OwnerStateComponent
  StateComponentForMissingStateFact(MissingStateFactKind kind);
  /// Return the terminal fallback proof failure for a component-specific
  /// missing producer fact.  The caller supplies the detail recorded when the
  /// owner summary was built so diagnostics identify the missing fact, not
  /// merely the fallback algorithm that noticed it.
  static TerminalFallbackProofFailure
  MissingStateFactTerminalFailure(MissingStateFactKind kind, StringRef detail);
  /// Return the terminal fallback proof failure for an undischargeable suffix
  /// observer of `component`.
  static TerminalFallbackProofFailure
  SuffixStabilityTerminalFailureForComponent(OwnerStateComponent component);
  /// Return the state component explicitly named by a typed suffix-stability
  /// witness, or Unknown when the witness is absent/malformed.
  static OwnerStateComponent
  ComponentNamedBySuffixStabilityWitness(const SuffixStabilityWitness &witness);
  /// Validate that a typed suffix-stability witness names the exact state
  /// component that the gateway request is checking.  This guards against
  /// treating a generic enum label as a proof.
  static bool
  SuffixStabilityWitnessNamesComponent(const SuffixStabilityWitness &witness,
                                       OwnerStateComponent component);
  /// Check one state transition through the uniform gateway.
  ///
  /// This is the only decision tree that is allowed to decide whether a changed
  /// state component can cross into a preserved suffix.  Callers name the
  /// component and typed witness directly instead of routing through
  /// component-specific approval wrappers.
  StateTransitionProof CheckStateTransitionAcrossEditBoundary(
      const StateTransitionGatewayRequest &request) const;
  /// Convenience overload for the uniform state-transition gateway.
  StateTransitionProof CheckStateTransitionAcrossEditBoundary(
      const OwnerStateBoundary &boundary, OwnerStateComponent component,
      StateMutationKind mutation, SuffixStabilityWitness witness,
      llvm::StringRef stage, llvm::StringRef detail,
      bool requireKnownObserver) const;
  /// Build one typed suffix-stability witness for the uniform state gateway.
  ///
  /// Use one typed witness carrier for all state components: the theorem fact
  /// is the component named in the witness, not which helper happened to create
  /// it. Terminal witnesses use
  /// SuffixStabilityTerminalFailureForComponent(component), keeping the
  /// component-specific failed obligation but eliminating side-approval APIs.
  static SuffixStabilityWitness BuildStateTransitionWitness(
      SuffixStabilityWitnessKind kind, OwnerStateComponent component,
      const OwnerStateBoundary &boundary, llvm::StringRef detail);

  /// Return the source/token boundary for one producer-backed counter event.
  static OwnerStateBoundary
  CounterStateBoundaryForEvent(const CounterEventIdentity &event);
  /// Format one counter event for typed witness diagnostics.
  static std::string
  FormatCounterEventForWitness(const CounterEventIdentity &event);
  /// Return true when a component belongs to logical line/file state.
  static bool IsLineControlStateComponent(OwnerStateComponent component);
  /// Format line-control state facts for the current proof model witness keys
  /// without reparsing source text.
  static std::string
  FormatLineControlEventForWitness(const LineControlStateIdentity &event);
  /// Format builtin-location observations for the current proof model witness
  /// keys without reparsing source text.
  static std::string FormatBuiltinLocationObservationForWitness(
      const BuiltinLocationObservation &observation);
  /// Build the source/token boundary for one include directive.
  static OwnerStateBoundary
  IncludeStateBoundaryForIncludeSite(const RefoldModel::IncludeItem &include);

  /// Return the coarse observer kind for a state component.
  static SuffixObservationKind
  ObservationKindForComponent(OwnerStateComponent component);
  /// Return the component-specific observer-site index for `component`.
  static ArrayRef<uint64_t>
  ObserverSiteIndexesForComponent(const OwnerStateGraph &graph,
                                  OwnerStateComponent component);
  /// True iff an observer summary contains `component`.
  static bool
  OwnerObserverSummaryObservesComponent(const OwnerObserverSummary &summary,
                                        OwnerStateComponent component);

  /// True iff a source-site record belongs to `owner`.
  ///
  /// `ownerIncludeId` is the producer's current include-instance owner for the
  /// source bytes.  The optional conditional arm on `owner` is treated as an
  /// additional restriction: when present, the site must lie inside that arm.
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

  /// This run's recorded owner-state graph, or null to always build one.
  ///
  /// Borrowed, never owned: the narrowing loop that owns the memo outlives
  /// every engine it hands it to.  See `OwnerStateGraphMemo`.
  OwnerStateGraphMemo *ownerStateGraphMemo_ = nullptr;

  /// Lazily built owner-state census indexes.
  ///
  /// The proof service owns these caches because they are derived from the
  /// immutable model/source inputs and are consumed only by owner-state graph,
  /// delta, and suffix-observer queries.
  mutable std::optional<OwnerStateFactIndex> ownerStateFactIndexCache_;
  mutable std::optional<OwnerStateGraph> ownerStateGraphCache_;
  mutable std::map<OwnerStateDeltaCacheKey, OwnerStateDelta>
      ownerStateDeltaCache_;
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

/// Return true iff a retained raw-lexer comment token has a complete spelling.
///
/// Clang's raw lexer owns the hard parts of raw-source normalization, including
/// escaped-newline handling and language-mode details.  This helper is only a
/// defensive completeness check before treating a comment token as ignorable
/// source trivia: block comments must have a real closing delimiter, while line
/// comments are complete at either newline or end-of-buffer.
bool rawLexerCommentTokenIsComplete(StringRef spelling);

/// Return true iff `text` is only whitespace and complete C/C++ comments.
///
/// This helper is intentionally shared because balanced pragma-state proof is
/// used by TU and header owner-envelope code.  It is a thin policy wrapper over
/// the raw-lexer trivia predicate: incomplete comments or any token spelling
/// reject the island, forcing the caller back to a wider structural proof or
/// terminal fallback.
bool sourceTextIsOnlyWhitespaceAndCompleteComments(StringRef text,
                                                   const LangOptions &lang);

/// Strictly parse the diagnostic pragma-state sublanguage admitted by the
/// balanced-island proof.
///
/// The proof does not try to understand arbitrary pragmas.  It accepts only the
/// two Clang-supported diagnostic namespaces whose state model is a stack
/// (`clang diagnostic` and `GCC diagnostic`) and only the push/pop/settings
/// grammar whose net state can be checked locally.  Anything else remains
/// side-effect-bearing and therefore fail-closed.
std::optional<ParsedDiagnosticPragmaStateDirective>
parseDiagnosticPragmaStateDirective(StringRef text, const LangOptions &lang);

/// Return true iff two non-empty half-open intervals overlap.
bool intervalsOverlap(uint64_t beginA, uint64_t endA, uint64_t beginB,
                      uint64_t endB);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H

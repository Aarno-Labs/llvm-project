//===--- RefoldMacroStateRepairPlanner.cpp ----------------------*- C++ -*-===//
//
// This file implements the macro-state repair planner used after TU-level
// source edits have been staged.  The public planner owns only dependency
// injection and thin entry points.  MacroStateRepairContext owns the shared
// directive indexes, final TU edit interval queries, include ancestry checks,
// macro-state transition proofs, and repair mutations used by all repair
// phases.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroStateRepairPlanner.h"

#include "core/RefoldLog.h"
#include "edit/RefoldPatchTypes.h"
#include "edit/RefoldTextEditAssembler.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroStateProof.h"
#include "proof/RefoldNeutralityProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTerminalProofSink.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "source/TokenTextHelpers.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

using MacroDirectiveSourceInterval = MacroStateDirectiveLineInterval;
using NamedMacroDirectiveRef =
    RefoldMacroStateRepairPlanner::NamedMacroDirectiveRef;
using MacroStateRepairPlan =
    RefoldMacroStateRepairPlanner::MacroStateRepairPlan;
using MacroStateRepairRequest =
    RefoldMacroStateRepairPlanner::MacroStateRepairRequest;
using Dependencies = RefoldMacroStateRepairPlanner::Dependencies;

enum class MacroStatePreservationPlacement {
  BeforeReplacement,
  InsideReplacement,
  AfterReplacement,
  AdvancedBeforeReplacement,
};

struct MacroStatePreservation {
  const RefoldModel::MacroDirective *directive = nullptr;
  MacroStatePreservationPlacement placement =
      MacroStatePreservationPlacement::BeforeReplacement;
  size_t replacementOffset = 0;
};

/// Exact physical source surface that embodies one macro-state transition.
///
/// This record is private to the specialized repair planner. It is deliberately
/// separate from `TUByteSpanPlan`: direct token-span planning may prove that an
/// exact transition exists, but only this planner may decide whether the final
/// edit preserves, replays, or moves that transition and mint the corresponding
/// protected-source capability.
enum class MacroStateTransitionSurface {
  DirectTUMacroDirective,
  OwningTUIncludeDirective,
};

struct MacroStateSourceTransition {
  const RefoldModel::MacroDirective *directive = nullptr;
  MacroDirectiveSourceInterval interval;
  std::string text;
  MacroStateTransitionSurface surface =
      MacroStateTransitionSurface::DirectTUMacroDirective;
};

/// Semantic disposition proved for one consumed physical transition.
///
/// The enum names why the original protected interval may be consumed.  It is
/// intentionally independent of source-envelope shape: two edits may cover the
/// same bytes while having different state semantics, and only the disposition
/// proved by this planner is allowed to mint macro-state authority.
enum class MacroStateTransitionDisposition : uint8_t {
  Unknown,
  ReplayedBeforeReplacement,
  ReplayedInsideReplacement,
  ReplayedAfterReplacement,
  MovedEarlier,
  MovedLater,
  Materialized,
};

StringRef macroStateTransitionDispositionName(
    MacroStateTransitionDisposition disposition) {
  switch (disposition) {
  case MacroStateTransitionDisposition::Unknown:
    return "unknown";
  case MacroStateTransitionDisposition::ReplayedBeforeReplacement:
    return "replayed-before-replacement";
  case MacroStateTransitionDisposition::ReplayedInsideReplacement:
    return "replayed-inside-replacement";
  case MacroStateTransitionDisposition::ReplayedAfterReplacement:
    return "replayed-after-replacement";
  case MacroStateTransitionDisposition::MovedEarlier:
    return "moved-earlier";
  case MacroStateTransitionDisposition::MovedLater:
    return "moved-later";
  case MacroStateTransitionDisposition::Materialized:
    return "materialized";
  }
  llvm_unreachable("invalid macro-state transition disposition");
}

/// Complete theorem record authorizing one exact macro-state source interval.
///
/// This private record is the boundary between macro-state proof and source
/// capability.  It names the producer directive, its exact physical interval,
/// original and final state boundaries, mutation, complete suffix-observer
/// census, and the reason for the disposition.  `TUByteSpanPlan` never carries
/// this record and ordinary direct-TU planning cannot construct it.
struct ProvenMacroStateSourceTransition {
  MacroStateSourceTransition source;
  OwnerStateBoundary originalBoundary;
  OwnerStateBoundary finalBoundary;
  StateMutationKind mutation = StateMutationKind::Unknown;
  MacroStateTransitionDisposition disposition =
      MacroStateTransitionDisposition::Unknown;
  /// Replacement-local byte boundary when the transition is replayed inside
  /// the source carrier.  Together with `finalBoundary`, this distinguishes
  /// before/inside/after placements that share the same original source span.
  std::optional<size_t> finalReplacementOffset = std::nullopt;
  SuffixObserverQueryResult relevantObservers;
  StateTransitionProof proof;
  std::string reason;

  bool IsComplete() const {
    return source.directive && source.interval.begin < source.interval.end &&
           originalBoundary.hasSourceBoundary &&
           originalBoundary.source.IsComplete() &&
           finalBoundary.hasSourceBoundary &&
           finalBoundary.source.IsComplete() &&
           mutation != StateMutationKind::Unknown &&
           disposition != MacroStateTransitionDisposition::Unknown &&
           !proof.failure && !proof.suffixWitnesses.empty() &&
           !reason.empty();
  }
};

struct MacroStateGapCarryCandidate {
  const RefoldModel::MacroDirective *directive = nullptr;
  MacroDirectiveSourceInterval interval;
  std::string preservationText;
  std::string name;
};

struct SyntheticUndefCandidate {
  const RefoldModel::MacroDirective *definition = nullptr;
  MacroDirectiveSourceInterval interval;
  std::string name;
};

class MacroStateRepairContext {
public:
  /// Creates a repair context and builds the shared macro directive index
  /// exactly once for that planner invocation.
  MacroStateRepairContext(const Dependencies &deps,
                          const MacroStateRepairRequest &request,
                          MacroStateRepairPlan &plan)
      : deps_(deps), plan_(plan),
        dispatcher_(*request.structuralHunkDispatcher),
        tuEdits_(*request.tuEdits), tuPath_(request.tuPath),
        tuBytes_(request.tuBytes),
        ownersMustExpand_(request.ownersMustExpand) {
    BuildDirectiveIndexOnce();
  }

  /// Runs the initial macro-state liveness repair phases over the staged final
  /// TU edits and records preservation decisions in the plan.
  bool RunInitialRepair();
  /// Applies delayed definition carry repairs after replacement edit boundaries
  /// have stabilized.
  void CarryObservedGapDefinitionsAfterReplacements();
  /// Appends required macro definition repairs to a materialized include
  /// replacement when surviving downstream observers still need them.
  bool RepairConsumedDefinitionsForMaterializedInclude(
      const RefoldModel::IncludeItem &materializedInclude,
      uint64_t materializedSiteBegin, uint64_t materializedSiteEnd,
      std::string &replacementText);

private:
  /// Returns the immutable refold model used for macro/include queries.
  const RefoldModel &Model() const { return *deps_.model; }
  /// Returns the path identity service used for physical source comparisons.
  const RefoldPathIdentity &PathIdentity() const { return *deps_.pathIdentity; }
  /// Returns the macro topology service used to inspect expansion relations.
  const RefoldMacroTopology &MacroTopology() const {
    return *deps_.macroTopology;
  }
  /// Returns token-text analysis helpers for replacement observation checks.
  const RefoldTokenTextAnalysis &TokenTextAnalysis() const {
    return *deps_.tokenTextAnalysis;
  }
  /// Returns macro-state proof helpers used to locate observations and
  /// witnesses.
  const RefoldMacroStateProof &MacroStateProof() const {
    return *deps_.macroStateProof;
  }
  /// Returns owner-state proof machinery used to validate repair transitions.
  RefoldOwnerStateProof &OwnerStateProof() const {
    return *deps_.ownerStateProof;
  }
  /// Returns the proof lattice used when restaging conservative TU edits.
  RefoldProofLattice &ProofLattice() const { return *deps_.proofLattice; }
  /// Returns the macro patch planner used to detect existing macro surfaces.
  RefoldMacroPatchPlanner &MacroPatchPlanner() const {
    return *deps_.macroPatchPlanner;
  }
  /// Returns the edit assembler used to restage and certify repair edits.
  RefoldTextEditAssembler &TextEditAssembler() const {
    return *deps_.textEditAssembler;
  }
  /// Returns the terminal proof sink used for conservative fallback evidence.
  RefoldTerminalProofSink &TerminalSink() const { return *deps_.terminalSink; }
  /// Returns lexer language options used by boundary and token scanners.
  const clang::LangOptions &LexLang() const { return *deps_.lexLang; }

  /// Populates the plan's macro directive lookup tables if they have not
  /// already been built for the current repair plan.
  void BuildDirectiveIndexOnce();

  /// Returns whether two half-open source intervals overlap.
  static bool SourceIntervalsOverlap(uint64_t aBegin, uint64_t aEnd,
                                     uint64_t bBegin, uint64_t bEnd);
  /// Returns whether a source interval overlaps any final TU edit.
  bool IntervalOverlapsFinalTUEdit(uint64_t begin, uint64_t end) const;
  /// Finds the final TU edit that fully contains a source interval, if any.
  std::optional<size_t> FinalTUEditContainingInterval(uint64_t begin,
                                                      uint64_t end) const;
  /// Returns whether a source range overlaps a final TU edit other than the
  /// explicitly excluded edit index.
  bool SourceRangeOverlapsFinalTUEditExcept(uint64_t begin, uint64_t end,
                                            size_t exceptEditIndex) const;

  /// Finds the outermost TU-visible include site for an include ownership
  /// chain.
  const RefoldModel::IncludeItem *
  OutermostOwningIncludeSiteInTU(std::optional<uint64_t> includeId) const;
  /// Finds the TU-visible include site that owns a macro directive, if any.
  const RefoldModel::IncludeItem *
  OwningIncludeSiteInTU(const RefoldModel::MacroDirective &directive) const;
  /// Returns the original include directive text for an include item.
  StringRef IncludeDirectiveText(const RefoldModel::IncludeItem &inc) const;
  /// Returns whether an include directive is preserved at a line-start boundary
  /// inside a replacement edit.
  bool
  IncludeDirectiveAppearsAtLineStart(const TextEdit &edit,
                                     const RefoldModel::IncludeItem &inc) const;
  /// Returns whether a TU-visible include site survives or is exactly preserved
  /// by the final TU edit stream.
  bool
  IncludeSitePreservedByFinalTUEdit(const RefoldModel::IncludeItem &inc) const;
  /// Returns whether the include ancestry needed for an include-owned directive
  /// remains represented after final TU edits.
  bool IncludeAncestrySitePreservedByFinalTUEdit(
      const RefoldModel::MacroDirective &directive) const;

  /// Computes the full physical source interval occupied by a macro directive.
  std::optional<MacroDirectiveSourceInterval> MacroDirectiveFullSourceInterval(
      const RefoldModel::MacroDirective &directive) const;
  /// Returns whether a macro-state directive appears at a line-start boundary
  /// in a replacement edit.
  bool MacroStateDirectiveAppearsAtLineStart(
      const TextEdit &edit, const RefoldModel::MacroDirective &directive) const;
  /// Finds the final TU edit that fully contains a macro directive, if any.
  std::optional<size_t> FinalTUEditContainingMacroDirective(
      const RefoldModel::MacroDirective &directive) const;
  /// Returns whether a macro directive is touched by any final TU edit.
  bool MacroDirectiveTouchedByTUEdit(
      const RefoldModel::MacroDirective &directive) const;
  /// Builds the directive spelling used when preserving a consumed macro-state
  /// transition in a repair edit.
  std::string DirectiveTextForPreservation(
      const RefoldModel::MacroDirective &directive) const;

  /// Returns the directive's exact source spelling, or its recorded canonical
  /// text when the map has no physical extent or the file cannot be read.
  StringRef
  ExactDirectiveSourceText(const RefoldModel::MacroDirective &directive) const;

  /// Reconstructs the macro-state transition represented by a source directive.
  /// Memoized; see the definition for why the answer is stable per directive.
  std::optional<MacroStateSourceTransition> MacroStateSourceTransitionFor(
      const RefoldModel::MacroDirective &directive) const;
  /// Uncached body of `MacroStateSourceTransitionFor()`.
  std::optional<MacroStateSourceTransition> ComputeMacroStateSourceTransition(
      const RefoldModel::MacroDirective &directive) const;
  /// Return a point boundary in the final TU source carrier.
  OwnerStateBoundary FinalTUStateBoundary(uint64_t sourceOffset) const;
  /// Prove one complete macro-state disposition before any source capability is
  /// minted for the consumed transition.
  std::optional<ProvenMacroStateSourceTransition>
  ProveMacroStateSourceTransition(
      const RefoldModel::MacroDirective &directive, StateMutationKind mutation,
      MacroStateTransitionDisposition disposition,
      const OwnerStateBoundary &finalBoundary,
      std::optional<size_t> finalReplacementOffset,
      SuffixStabilityWitnessKind witnessKind, StringRef stage, StringRef reason,
      bool requireKnownObserver = false) const;
  /// Mint the exact protected-source capability carried by one already-proved
  /// transition.  Raw source intervals are deliberately not accepted.
  /// Return the include this transition would delete when its subtree carries,
  /// anywhere, a pragma this repair cannot preserve.
  std::optional<uint64_t> IncludeSubtreeCarryingUnmodeledPragma(
      const ProvenMacroStateSourceTransition &transition) const;

  bool AuthorizeMacroStateSourceTransition(
      TextEdit &edit,
      const ProvenMacroStateSourceTransition &transition) const;
  /// Finds the active definition for a macro name at a physical source offset.
  const RefoldModel::MacroDirective *
  ActiveDefinitionAtSourceOffset(StringRef macroName, uint64_t offset) const;
  /// Returns whether a consumed macro-state directive can be delayed after a
  /// replacement edit without changing observer semantics.
  bool MacroStateDirectiveCanBeDelayedAfterEdit(
      const TextEdit &edit, const RefoldModel::MacroDirective &directive) const;
  /// Returns whether a definition is already available before a source offset.
  bool MacroStateDefinitionAvailableBeforeSourceOffset(
      const RefoldModel::MacroDirective &definition, uint64_t offset) const;

  /// Resolve an expansion-chain node to its outermost physical invocation.
  const RefoldModel::MacroInvocation *PhysicalRootInvocation(
      const RefoldModel::MacroInvocation &invocation) const;
  /// Return whether an accepted patch or complete owner edit materializes the
  /// physical callsite and therefore removes its definition dependency.
  bool PhysicalCallsiteIsMaterialized(
      const RefoldModel::MacroInvocation &invocation) const;
  /// Authorize a consumed definition only when every producer-linked physical
  /// callsite has an accepted materialized disposition.
  bool AuthorizeMaterializedDefinitionTransitions();

  /// Finds the first replacement-text observation of a preserved definition.
  std::optional<size_t> FirstReplacementObservationOffset(
      const TextEdit &edit, const RefoldModel::MacroDirective &definition,
      StringRef macroName) const;
  /// Returns whether replacement text observes the preserved definition state.
  bool ReplacementObservesPreservedDefinition(
      const TextEdit &edit, const RefoldModel::MacroDirective &definition,
      StringRef macroName) const;
  /// Finds the replacement line start that precedes a macro observation.
  std::optional<size_t>
  ReplacementLineStartBeforeObservation(StringRef replacement,
                                        size_t observationOffset) const;
  /// Finds the line start after the final replacement observation of a macro.
  std::optional<size_t> ReplacementLineStartAfterFinalObservation(
      const TextEdit &edit, const RefoldModel::MacroDirective &definition,
      StringRef macroName, size_t firstObservationOffset) const;
  /// Returns whether a replacement suffix boundary can safely host a directive
  /// line without merging into source text.
  bool ReplacementSuffixBoundaryAllowsDirectiveLine(const TextEdit &edit) const;
  /// Returns whether an edit already carries a macro patch surface in B-state.
  bool EditHasMacroPatchSurfaceInBMacroState(const TextEdit &edit) const;
  /// Returns whether a direct-callee macro patch intentionally depends on a
  /// definition remaining before the rewritten invocation.
  bool DirectCalleePatchRequiresDefinitionBeforeReplacement(
      const TextEdit &edit, const RefoldModel::MacroDirective &definition,
      StringRef macroName) const;
  /// Returns whether this edit's replacement folds preserved TU source in with
  /// its B payload, so the replacement is not wholly in B's macro state.
  bool EditClosesOverPreservedTUSource(const TextEdit &edit) const;

  /// Builds the owner-state boundary immediately after a macro directive.
  OwnerStateBoundary MacroDirectiveSuffixBoundary(
      const RefoldModel::MacroDirective &directive) const;
  /// Validates a macro-state transition across an edit boundary using an
  /// explicit suffix-stability witness.
  StateTransitionProof
  CheckMacroStateWithWitness(const OwnerStateBoundary &boundary,
                             StateMutationKind mutation,
                             SuffixStabilityWitness witness, StringRef stage,
                             StringRef detail, bool requireKnownObserver) const;
  /// Validates proof evidence for a repaired macro-state transition.
  StateTransitionProof
  CheckMacroStateRepaired(const RefoldModel::MacroDirective &directive,
                          StateMutationKind mutation, StringRef stage,
                          StringRef detail,
                          bool requireKnownObserver = false) const;
  /// Validates terminal fallback evidence for a macro-state transition.
  ///
  /// `failingOwnerId` names the region whose realization the check condemns,
  /// when the caller knows one that is more specific than the directive's own
  /// include instance -- the materialized include that consumes the definition,
  /// or the invocation whose surviving callsite observes it.  That region is
  /// what the fallback ladder gives up instead of the translation unit; leaving
  /// it absent falls back to the include instance owning the directive, and a
  /// directive in the translation unit itself stays honestly unattributed.
  StateTransitionProof
  CheckMacroStateTerminal(const RefoldModel::MacroDirective &directive,
                          StateMutationKind mutation, StringRef stage,
                          StringRef detail, bool requireKnownObserver = true,
                          std::optional<uint64_t> failingOwnerId =
                              std::nullopt) const;
  /// Maps a preservation placement to the corresponding owner-state mutation.
  StateMutationKind MutationForMacroStatePreservationPlacement(
      MacroStatePreservationPlacement placement) const;
  /// Returns a stable diagnostic name for a preservation placement.
  StringRef MacroStatePreservationPlacementName(
      MacroStatePreservationPlacement placement) const;

  /// Computes a delayed macro-state boundary after an edit when the source
  /// suffix can safely carry the directive line.
  std::optional<uint64_t> DelayedTransitionBoundaryAfterEdit(
      size_t editIndex, const RefoldModel::MacroDirective &definition,
      StringRef macroName) const;
  /// Attempts to move a consumed undef before an observed replacement that
  /// depends on the previous live definition.
  std::optional<MacroStatePreservationPlacement>
  TryAdvanceConsumedUndefBeforeObservedReplacement(
      size_t editIndex, const RefoldModel::MacroDirective &undefDirective,
      StringRef macroName,
      const RefoldModel::MacroDirective &previousDefinition,
      size_t firstObservationOffset);
  /// Attempts to widen an edit to a delayed boundary that preserves macro-state
  /// liveness without creating an extra directive line.
  bool TryWidenEditToDelayedMacroStateBoundary(
      size_t editIndex, const RefoldModel::MacroDirective &definition,
      StringRef macroName);
  /// Queues a macro-state directive preservation for an edit when proof and
  /// placement constraints allow it.
  std::optional<MacroStatePreservationPlacement>
  TryQueueMacroStateDirectivePreservation(
      size_t editIndex, const RefoldModel::MacroDirective &directive,
      StringRef macroName,
      const RefoldModel::MacroDirective *observedDefinition);

  /// Promote a repaired edit from ordinary direct-TU provenance to the
  /// specialized macro-state repair carrier.  Ordinary TUByteSpan carriers are
  /// removed rather than retained beside the new authority, so final emission
  /// cannot repurpose their weaker theorem after edit normalization.
  void PromoteToSpecializedMacroStateRepairCarrier(TextEdit &edit);
  /// Restages a conservative TU edit after a repair changes its byte range or
  /// replacement text.
  void RestageConservativeTUEdit(
      TextEdit &edit, uint64_t start, uint64_t end, StringRef replacement,
      ArrayRef<ProvenMacroStateSourceTransition> repairedTransitions = {});
  /// Attaches conservative TU proof carrier metadata to a repaired edit.
  void AttachConservativeTUCarrier(TextEdit &edit);

  /// Advances consumed undef transitions before replacements that observe the
  /// prior definition state.
  void AdvancePreservedUndefsBeforeObservedReplacements();
  /// Synthesizes undef repairs before observed gap definitions when required to
  /// preserve macro-state partitioning.
  void SynthesizeUndefBeforeObservedGapDefinitions();
  /// Repairs definitions required by surviving callsites after final TU edits.
  void RepairSurvivingDefinitionCallsites();
  /// Preserves consumed undef directives that remain semantically required and
  /// returns the number of applied repairs.
  size_t PreserveConsumedUndefs();
  /// Applies queued macro-state preservations and authorizes only the exact
  /// transitions discharged by those repairs.
  bool ApplyQueuedMacroStatePreservations();

  /// Returns whether a definition directive is consumed or altered by TU edits.
  bool DefinitionDirectiveTouchedByTUEdit(
      const RefoldModel::MacroDirective &directive) const;
  /// Finds the TU-visible include site that owns a macro invocation, if any.
  const RefoldModel::IncludeItem *OwningIncludeSiteForInvocationInTU(
      const RefoldModel::MacroInvocation &invocation) const;
  /// Returns whether an include-owned invocation survives the final TU edits.
  bool IncludeOwnedInvocationSurvivesTUEdits(
      const RefoldModel::MacroInvocation &invocation) const;
  /// Returns whether an invocation callsite remains present after final edits.
  bool InvocationCallsiteSurvivesTUEdits(
      const RefoldModel::MacroInvocation &invocation) const;
  /// Finds a conservative fallback active definition for an invocation.
  const RefoldModel::MacroDirective *FallbackActiveDefinitionForInvocation(
      const RefoldModel::MacroInvocation &invocation) const;
  /// Finds the active macro definition that should govern an invocation.
  const RefoldModel::MacroDirective *ActiveDefinitionForInvocation(
      const RefoldModel::MacroInvocation &invocation) const;
  /// Returns whether another same-name transition survives and can preserve the
  /// required macro-state boundary.
  bool DefinitionHasOtherSurvivingSameNameTransition(
      const RefoldModel::MacroDirective &definition, StringRef macroName) const;
  /// Finds the previous live definition before an undef directive.
  const RefoldModel::MacroDirective *PreviousLiveDefinitionBeforeDirective(
      const NamedMacroDirectiveRef &undefRef) const;
  /// Returns whether a macro directive remains represented after TU edits.
  bool
  DirectiveSurvivesTUEdits(const RefoldModel::MacroDirective &directive) const;

  /// Returns whether a materialized include subtree owns a macro directive.
  bool MaterializedIncludeSubtreeOwnsMacroDirective(
      const RefoldModel::IncludeItem &root,
      const RefoldModel::MacroDirective &directive) const;
  /// Returns whether a materialized replacement preserves the include ancestry
  /// required for a macro directive.
  bool MaterializedReplacementPreservesDirectiveAncestry(
      const RefoldModel::IncludeItem &root,
      const RefoldModel::MacroDirective &directive,
      const TextEdit &replacementEdit) const;
  /// Returns whether an invocation still survives after the materialized
  /// include replacement site.
  bool InvocationSurvivesAfterMaterializedInclude(
      const RefoldModel::MacroInvocation &invocation,
      const RefoldModel::IncludeItem &materializedInclude,
      uint64_t materializedSiteEnd) const;
  /// Returns whether `definition`'s recorded replacement list names
  /// `macroName`.
  bool DefinitionReplacementListNamesMacro(
      const RefoldModel::MacroDirective &definition,
      llvm::StringRef macroName) const;
  /// Returns whether a materialized include must carry a definition afterward
  /// to satisfy surviving downstream observers.
  bool MaterializedIncludeNeedsDefinitionAfterward(
      const RefoldModel::IncludeItem &materializedInclude,
      uint64_t materializedSiteEnd,
      const RefoldModel::MacroDirective &definition) const;

  const Dependencies &deps_;
  MacroStateRepairPlan &plan_;
  RefoldStructuralHunkDispatcher &dispatcher_;
  std::vector<TextEdit> &tuEdits_;
  StringRef tuPath_;
  StringRef tuBytes_;
  /// Regions the fallback ladder ruled out; see MacroStateRepairRequest.
  const llvm::DenseSet<uint64_t> *ownersMustExpand_ = nullptr;
  std::map<size_t, SmallVector<MacroStatePreservation, 4>>
      macroStatePreservationsByEdit_;
  /// Exact source spelling per macro-directive id, read on first use.
  mutable llvm::DenseMap<uint64_t, std::string> exactDirectiveSourceText_;
  /// Memoized `MacroStateSourceTransitionFor()` results, keyed on directive id.
  /// Absent transitions are cached too: proving one absent costs the same
  /// interval recovery as proving one present.
  mutable llvm::DenseMap<uint64_t, std::optional<MacroStateSourceTransition>>
      macroStateSourceTransition_;
};

void MacroStateRepairContext::BuildDirectiveIndexOnce() {
  if (plan_.directiveIndexBuilt)
    return;

  plan_.macroDirectiveById.clear();
  plan_.namedMacroDirectives.clear();
  plan_.namedMacroDirectivesByName.clear();

  // Build a name-indexed view of macro-state directives that can affect later
  // preserved source.  The producer records the controlled #define/#undef macro
  // name directly, so this proof never reparses directive text merely to
  // recover the macro-state key.  Empty names are malformed proof data and
  // ignored fail-closed.
  for (const RefoldModel::MacroDirective &directiveLocal :
       Model().GetMacroDirectives()) {
    if (directiveLocal.subkind != "#define" &&
        directiveLocal.subkind != "#undef")
      continue;
    if (directiveLocal.name.empty())
      continue;
    NamedMacroDirectiveRef ref{&directiveLocal, directiveLocal.name};
    plan_.macroDirectiveById[directiveLocal.id] = ref;
    plan_.namedMacroDirectives.push_back(ref);
  }

  // Keep the ordered view deterministic so macro-state damage intervals can be
  // computed by walking directives in their original source order.
  llvm::sort(plan_.namedMacroDirectives, [](const NamedMacroDirectiveRef &lhs,
                                            const NamedMacroDirectiveRef &rhs) {
    return lhs.directive->id < rhs.directive->id;
  });

  // Group by controlled name after the sort, so each bucket lists positions in
  // ascending directive id exactly as the ordered view does.  Every macro-state
  // question below is asked about a single name; answering one by walking the
  // whole ordered view costs a string comparison per directive in the unit.
  for (uint32_t position = 0; position < plan_.namedMacroDirectives.size();
       ++position) {
    plan_.namedMacroDirectivesByName[plan_.namedMacroDirectives[position].name]
        .push_back(position);
  }

  plan_.directiveIndexBuilt = true;
}

bool MacroStateRepairContext::SourceIntervalsOverlap(uint64_t aBegin,
                                                     uint64_t aEnd,
                                                     uint64_t bBegin,
                                                     uint64_t bEnd) {
  return aBegin < bEnd && bBegin < aEnd;
}

bool MacroStateRepairContext::IntervalOverlapsFinalTUEdit(uint64_t begin,
                                                          uint64_t end) const {
  for (const TextEdit &edit : tuEdits_) {
    if (SourceIntervalsOverlap(begin, end, edit.start, edit.end))
      return true;
  }
  return false;
}

std::optional<size_t>
MacroStateRepairContext::FinalTUEditContainingInterval(uint64_t begin,
                                                       uint64_t end) const {
  for (size_t editIndex = 0; editIndex < tuEdits_.size(); ++editIndex) {
    const TextEdit &edit = tuEdits_[editIndex];
    if (edit.start <= begin && end <= edit.end)
      return editIndex;
  }
  return std::nullopt;
}

bool MacroStateRepairContext::SourceRangeOverlapsFinalTUEditExcept(
    uint64_t begin, uint64_t end, size_t exceptEditIndex) const {
  for (size_t editIndex = 0; editIndex < tuEdits_.size(); ++editIndex) {
    if (editIndex == exceptEditIndex)
      continue;
    const TextEdit &edit = tuEdits_[editIndex];
    if (SourceIntervalsOverlap(begin, end, edit.start, edit.end))
      return true;
  }
  return false;
}

const RefoldModel::IncludeItem *
MacroStateRepairContext::OutermostOwningIncludeSiteInTU(
    std::optional<uint64_t> includeId) const {
  if (!includeId)
    return nullptr;

  const RefoldModel::IncludeItem *cur = Model().GetIncludeById(*includeId);
  while (cur) {
    if (PathIdentity().PathsEqual(cur->sitePath, tuPath_))
      return cur;
    if (!cur->parent)
      return nullptr;
    cur = Model().GetIncludeById(*cur->parent);
  }
  return nullptr;
}

const RefoldModel::IncludeItem *MacroStateRepairContext::OwningIncludeSiteInTU(
    const RefoldModel::MacroDirective &directive) const {
  return OutermostOwningIncludeSiteInTU(directive.ownerIncludeId);
}

StringRef MacroStateRepairContext::IncludeDirectiveText(
    const RefoldModel::IncludeItem &inc) const {
  if (!inc.text.empty())
    return inc.text;
  if (PathIdentity().PathsEqual(inc.sitePath, tuPath_) &&
      inc.siteE <= tuBytes_.size())
    return tuBytes_.slice(inc.siteB, inc.siteE);
  return StringRef();
}

bool MacroStateRepairContext::IncludeDirectiveAppearsAtLineStart(
    const TextEdit &edit, const RefoldModel::IncludeItem &inc) const {
  StringRef directiveText = IncludeDirectiveText(inc);
  return !directiveText.empty() &&
         stringutils::containsAtLineStartAfterIndent(edit.text, directiveText);
}

bool MacroStateRepairContext::IncludeSitePreservedByFinalTUEdit(
    const RefoldModel::IncludeItem &inc) const {
  std::optional<size_t> editIndex =
      FinalTUEditContainingInterval(inc.siteB, inc.siteE);
  if (!editIndex)
    return false;
  return IncludeDirectiveAppearsAtLineStart(tuEdits_[*editIndex], inc);
}

bool MacroStateRepairContext::IncludeAncestrySitePreservedByFinalTUEdit(
    const RefoldModel::MacroDirective &directive) const {
  if (!directive.ownerIncludeId)
    return false;

  const RefoldModel::IncludeItem *outerTU =
      OutermostOwningIncludeSiteInTU(directive.ownerIncludeId);
  if (!outerTU)
    return false;

  std::optional<size_t> editIndex =
      FinalTUEditContainingInterval(outerTU->siteB, outerTU->siteE);
  if (!editIndex)
    return false;
  const TextEdit &edit = tuEdits_[*editIndex];

  // Header materialization may remove the outer TU #include while preserving a
  // descendant include directive in the materialized replacement text.  Walk
  // from the directive's immediate owning include toward the TU include; if any
  // include in that ancestry is carried forward as a directive line, then the
  // macro-state transition remains available through source order and must not
  // be hoisted into the TU.
  const RefoldModel::IncludeItem *cur =
      Model().GetIncludeById(*directive.ownerIncludeId);
  while (cur) {
    if (IncludeDirectiveAppearsAtLineStart(edit, *cur))
      return true;
    if (!cur->parent)
      break;
    cur = Model().GetIncludeById(*cur->parent);
  }
  return false;
}

std::optional<MacroDirectiveSourceInterval>
MacroStateRepairContext::MacroDirectiveFullSourceInterval(
    const RefoldModel::MacroDirective &directive) const {
  return MacroStateProof().RecoverMacroStateDirectiveLineInterval(
      directive, tuPath_, tuBytes_, std::nullopt);
}

/// MacroDirective::text is rendered from the parsed MacroInfo, so it is not the
/// bytes any emitted source contains.  An empty replacement list renders with a
/// trailing space, runs of whitespace collapse to one space, and a
/// backslash-continued definition folds onto a single line.  Both searching an
/// emitted payload for a directive and re-emitting one need the real spelling,
/// which the producer's recorded physical extent identifies exactly.
///
/// The defining file is read on first use and cached.  A map without the
/// extent, or a file that cannot be read, falls back to the recorded text and
/// therefore to the previous behaviour.
StringRef MacroStateRepairContext::ExactDirectiveSourceText(
    const RefoldModel::MacroDirective &directive) const {
  auto cached = exactDirectiveSourceText_.find(directive.id);
  if (cached != exactDirectiveSourceText_.end())
    return cached->second;

  std::string text = directive.text.str();
  if (directive.directiveLineB && directive.directiveLineE &&
      *directive.directiveLineB < *directive.directiveLineE &&
      !directive.sitePath.empty()) {
    StringRef bytes;
    std::unique_ptr<llvm::MemoryBuffer> owned;
    if (PathIdentity().PathsEqual(directive.sitePath, tuPath_)) {
      bytes = tuBytes_;
    } else if (auto bufOrErr =
                   llvm::MemoryBuffer::getFile(directive.sitePath)) {
      owned = std::move(*bufOrErr);
      bytes = owned->getBuffer();
    }
    if (*directive.directiveLineE <= bytes.size()) {
      text = bytes.slice(*directive.directiveLineB, *directive.directiveLineE)
                 .str();
    }
  }

  return exactDirectiveSourceText_.try_emplace(directive.id, std::move(text))
      .first->second;
}

bool MacroStateRepairContext::MacroStateDirectiveAppearsAtLineStart(
    const TextEdit &edit, const RefoldModel::MacroDirective &directive) const {
  StringRef spelling = ExactDirectiveSourceText(directive);
  return !spelling.empty() &&
         stringutils::containsAtLineStartAfterIndent(edit.text, spelling);
}

std::optional<size_t>
MacroStateRepairContext::FinalTUEditContainingMacroDirective(
    const RefoldModel::MacroDirective &directive) const {
  if (PathIdentity().PathsEqual(directive.sitePath, tuPath_)) {
    std::optional<size_t> editIndex =
        FinalTUEditContainingInterval(directive.siteB, directive.siteE);
    if (editIndex &&
        MacroStateDirectiveAppearsAtLineStart(tuEdits_[*editIndex], directive))
      return std::nullopt;
    return editIndex;
  }

  if (IncludeAncestrySitePreservedByFinalTUEdit(directive))
    return std::nullopt;

  if (const RefoldModel::IncludeItem *inc = OwningIncludeSiteInTU(directive)) {
    if (IncludeSitePreservedByFinalTUEdit(*inc))
      return std::nullopt;
    return FinalTUEditContainingInterval(inc->siteB, inc->siteE);
  }

  return std::nullopt;
}

bool MacroStateRepairContext::MacroDirectiveTouchedByTUEdit(
    const RefoldModel::MacroDirective &directive) const {
  if (PathIdentity().PathsEqual(directive.sitePath, tuPath_)) {
    for (const TextEdit &edit : tuEdits_) {
      if (!SourceIntervalsOverlap(directive.siteB, directive.siteE, edit.start,
                                  edit.end))
        continue;
      return !MacroStateDirectiveAppearsAtLineStart(edit, directive);
    }
    return false;
  }

  if (IncludeAncestrySitePreservedByFinalTUEdit(directive))
    return false;

  if (const RefoldModel::IncludeItem *inc = OwningIncludeSiteInTU(directive)) {
    if (IncludeSitePreservedByFinalTUEdit(*inc))
      return false;
    return IntervalOverlapsFinalTUEdit(inc->siteB, inc->siteE);
  }

  return false;
}

std::string MacroStateRepairContext::DirectiveTextForPreservation(
    const RefoldModel::MacroDirective &directive) const {
  // Re-emit the directive as it was written.  Preserving a directive is a
  // source-fidelity operation, so it must not rewrite the spelling into
  // MacroDirective::text's canonical rendering.
  std::string textLocal = ExactDirectiveSourceText(directive).str();
  if (textLocal.empty() || textLocal.back() != '\n')
    textLocal.push_back('\n');
  return textLocal;
}

std::optional<MacroStateSourceTransition>
MacroStateRepairContext::MacroStateSourceTransitionFor(
    const RefoldModel::MacroDirective &directive) const {
  // Memoized on directive id.
  //
  // Everything below reads state that is fixed for the whole repair pass: the
  // path-identity service, `tuPath_`/`tuBytes_`, the directive and its owning
  // include from the model, and `ownersMustExpand_`, which the driver sets
  // before the engine runs and never changes during it.  Nothing here consults
  // the mutable parts of `plan_`, so the answer for one directive cannot change
  // between calls within a context.  `ExactDirectiveSourceText()` is already
  // cached on the same key for the same reason.
  //
  // This is worth caching rather than merely tidy: the interval recovery below
  // raw-lexes the defining file's directive line, and the callers ask about the
  // same directives repeatedly while walking edits.
  auto cached = macroStateSourceTransition_.find(directive.id);
  if (cached != macroStateSourceTransition_.end())
    return cached->second;

  std::optional<MacroStateSourceTransition> computed =
      ComputeMacroStateSourceTransition(directive);
  macroStateSourceTransition_[directive.id] = computed;
  return computed;
}

std::optional<MacroStateSourceTransition>
MacroStateRepairContext::ComputeMacroStateSourceTransition(
    const RefoldModel::MacroDirective &directive) const {
  if (PathIdentity().PathsEqual(directive.sitePath, tuPath_) &&
      !directive.ownerIncludeId) {
    std::optional<MacroDirectiveSourceInterval> intervalLocal =
        MacroDirectiveFullSourceInterval(directive);
    if (!intervalLocal)
      return std::nullopt;
    return MacroStateSourceTransition{
        &directive, *intervalLocal, DirectiveTextForPreservation(directive),
        MacroStateTransitionSurface::DirectTUMacroDirective};
  }

  const RefoldModel::IncludeItem *inc = OwningIncludeSiteInTU(directive);
  if (!inc)
    return std::nullopt;

  // An include the ladder has ruled out is going to be materialized, so its
  // directive is not a surface this repair may consume.  Skipping the plan
  // rather than refusing it later is deliberate: a refusal at authorization
  // time abandons the pass with no result, while skipping simply leaves the
  // include to the realization that was already chosen for it.
  if (ownersMustExpand_ && ownersMustExpand_->count(inc->id))
    return std::nullopt;

  StringRef text = IncludeDirectiveText(*inc);
  if (text.empty())
    return std::nullopt;
  std::string spelling = text.str();
  if (spelling.empty() || spelling.back() != '\n')
    spelling.push_back('\n');
  MacroDirectiveSourceInterval interval;
  interval.begin = inc->siteB;
  interval.end = inc->siteE;
  return MacroStateSourceTransition{
      &directive, interval, std::move(spelling),
      MacroStateTransitionSurface::OwningTUIncludeDirective};
}

OwnerStateBoundary
MacroStateRepairContext::FinalTUStateBoundary(uint64_t sourceOffset) const {
  return OwnerStateBoundary::FromSource(
      OwnerSourceRange::From(tuPath_, sourceOffset, sourceOffset));
}

std::optional<ProvenMacroStateSourceTransition>
MacroStateRepairContext::ProveMacroStateSourceTransition(
    const RefoldModel::MacroDirective &directive, StateMutationKind mutation,
    MacroStateTransitionDisposition disposition,
    const OwnerStateBoundary &finalBoundary,
    std::optional<size_t> finalReplacementOffset,
    SuffixStabilityWitnessKind witnessKind, StringRef stage, StringRef reason,
    bool requireKnownObserver) const {
  std::optional<MacroStateSourceTransition> source =
      MacroStateSourceTransitionFor(directive);
  if (!source || mutation == StateMutationKind::Unknown ||
      disposition == MacroStateTransitionDisposition::Unknown ||
      !finalBoundary.hasSourceBoundary || !finalBoundary.source.IsValid() ||
      reason.empty()) {
    TerminalSink().RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionEditSetComposable,
            TerminalFallbackFailureReason::UncomposableEmissionEditSet),
        stage,
        llvm::formatv("incomplete specialized macro-state transition proof: "
                      "directive=#{0} mutation={1} reason={2}",
                      directive.id, mutation, reason)
            .str());
    return std::nullopt;
  }

  const OwnerStateBoundary originalBoundary =
      MacroDirectiveSuffixBoundary(directive);
  SuffixStabilityWitness witness =
      OwnerStateProof().BuildStateTransitionWitness(
          witnessKind, OwnerStateComponent::MacroState, originalBoundary,
          reason);
  StateTransitionProof proof = CheckMacroStateWithWitness(
      originalBoundary, mutation, std::move(witness), stage, reason,
      requireKnownObserver);
  if (proof.failure)
    return std::nullopt;

  ProvenMacroStateSourceTransition transition;
  transition.source = std::move(*source);
  transition.originalBoundary = originalBoundary;
  transition.finalBoundary = finalBoundary;
  transition.mutation = mutation;
  transition.disposition = disposition;
  transition.finalReplacementOffset = finalReplacementOffset;
  transition.relevantObservers = OwnerStateProof().FindSuffixObservers(
      originalBoundary, OwnerStateComponent::MacroState);
  transition.proof = std::move(proof);
  transition.reason = reason.str();
  if (!transition.IsComplete()) {
    TerminalSink().RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionEditSetComposable,
            TerminalFallbackFailureReason::UncomposableEmissionEditSet),
        stage,
        llvm::formatv("specialized macro-state transition proof remained "
                      "incomplete after gateway discharge: directive=#{0}",
                      directive.id)
            .str());
    return std::nullopt;
  }
  return transition;
}

std::optional<uint64_t>
MacroStateRepairContext::IncludeSubtreeCarryingUnmodeledPragma(
    const ProvenMacroStateSourceTransition &transition) const {
  // Which include instances does this consumed TU interval delete?  An
  // include-owned transition consumes the directive line itself, so match by
  // containment of the recorded site rather than by path alone: the same header
  // can be included several times and only the covered instances die here.
  llvm::SmallVector<uint64_t, 4> dyingRoots;
  for (const RefoldModel::IncludeItem &include : Model().GetIncludes()) {
    if (!PathIdentity().PathsEqual(include.sitePath, tuPath_))
      continue;
    if (include.siteB < transition.source.interval.begin ||
        include.siteE > transition.source.interval.end)
      continue;
    dyingRoots.push_back(include.id);
  }
  if (dyingRoots.empty())
    return std::nullopt;
  llvm::sort(dyingRoots);

  // Deleting an include deletes everything it entered, so the question is about
  // the whole subtree: a pragma two headers down dies exactly as one in the
  // directly included file does.
  llvm::SmallVector<uint64_t, 16> subtree(dyingRoots.begin(), dyingRoots.end());
  llvm::DenseSet<uint64_t> visited(dyingRoots.begin(), dyingRoots.end());
  for (size_t index = 0; index < subtree.size(); ++index) {
    for (const RefoldModel::IncludeItem &child : Model().GetIncludes()) {
      if (!child.parent || *child.parent != subtree[index])
        continue;
      if (visited.insert(child.id).second)
        subtree.push_back(child.id);
    }
  }

  for (const RefoldModel::PragmaDirective &pragma : Model().GetPragmas()) {
    if (pragmaDirectiveIsPragmaOnce(pragma, *deps_.lexLang))
      continue;

    // Prefer the producer's own ownership record.  Falling back to physical
    // path is deliberately conservative: a header reached both inside and
    // outside the dying subtree answers yes, which only declines a deletion.
    if (pragma.ownerIncludeId) {
      if (visited.count(*pragma.ownerIncludeId))
        return dyingRoots.front();
      continue;
    }
    for (uint64_t includeId : subtree) {
      const RefoldModel::IncludeItem *include = Model().GetIncludeById(includeId);
      const std::optional<StringRef> openedPath =
          include ? (include->openedPath ? include->openedPath
                                         : include->resolvedPath)
                  : std::nullopt;
      if (openedPath && PathIdentity().PathsEqual(*openedPath, pragma.sitePath))
        return dyingRoots.front();
    }
  }

  return std::nullopt;
}

bool MacroStateRepairContext::AuthorizeMacroStateSourceTransition(
    TextEdit &edit,
    const ProvenMacroStateSourceTransition &transition) const {
  if (!transition.IsComplete() ||
      transition.source.interval.begin < edit.start ||
      edit.end < transition.source.interval.end ||
      !PathIdentity().PathsEqual(transition.finalBoundary.source.path,
                                 tuPath_) ||
      transition.finalBoundary.source.begin !=
          transition.finalBoundary.source.end ||
      transition.finalBoundary.source.begin < edit.start ||
      edit.end < transition.finalBoundary.source.end ||
      (transition.finalReplacementOffset &&
       *transition.finalReplacementOffset > edit.text.size())) {
    TerminalSink().RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionEditSetComposable,
            TerminalFallbackFailureReason::UncomposableEmissionEditSet),
        "macro/state-repair",
        "specialized macro-state transition did not match its final edit "
        "carrier");
    return false;
  }

  const PreprocessingStructureKind macroStateKinds[] = {
      PreprocessingStructureKind::MacroDefine,
      PreprocessingStructureKind::MacroUndef};
  const PreprocessingStructureKind includeKinds[] = {
      PreprocessingStructureKind::Include,
      PreprocessingStructureKind::IncludeNext,
      PreprocessingStructureKind::Import};
  const PreprocessingStructureKind nestedMacroKinds[] = {
      PreprocessingStructureKind::PragmaOperator};

  const bool includeOwned =
      transition.source.surface ==
      MacroStateTransitionSurface::OwningTUIncludeDirective;

  // This authority deletes a whole `#include` on the strength of having
  // repaired the header's *macro* state.  That is only the state it can name.
  // A pragma is opaque: what it does is knowable only for the pragmas this tool
  // models, and there is no macro name to hoist it by, so an include whose
  // subtree carries one cannot be deleted on this proof.  Refusing here leaves
  // the ordinary realization lattice to materialize the include instead, where
  // the pragma survives in place -- inside whatever conditional guards it, and
  // with `#pragma once` still owned by the once-guard rewriter.
  if (includeOwned) {
    if (std::optional<uint64_t> pragmaOwner =
            IncludeSubtreeCarryingUnmodeledPragma(transition)) {
      // Refusing alone would abandon the pass with no result, so name the
      // include: the fallback ladder rules out preserving it, and the next
      // assembly materializes it instead of deleting it.  The pragma then
      // survives where it was written.
      TerminalSink().RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet,
              TerminalFallbackFailureContext::ForOwnerId(*pragmaOwner)),
          "macro/state-repair",
          "include-owned macro-state repair cannot preserve a pragma in the "
          "include subtree; the include must be materialized instead");
      return false;
    }
  }
  const ArrayRef<PreprocessingStructureKind> allowedKinds =
      includeOwned ? ArrayRef<PreprocessingStructureKind>(includeKinds)
                   : ArrayRef<PreprocessingStructureKind>(macroStateKinds);
  const ArrayRef<PreprocessingStructureKind> allowedNestedKinds =
      includeOwned ? ArrayRef<PreprocessingStructureKind>()
                   : ArrayRef<PreprocessingStructureKind>(nestedMacroKinds);
  const bool authorized =
      TextEditAssembler().AuthorizeExactProtectedSourceInterval(
          edit,
          includeOwned
              ? ProtectedSourceEditAuthorityKind::IncludeOwnedMacroStateRepair
              : ProtectedSourceEditAuthorityKind::MacroStateRepair,
          tuPath_, std::nullopt, tuBytes_, transition.source.interval.begin,
          transition.source.interval.end, allowedKinds, allowedNestedKinds);
  if (authorized) {
    REFOLD_LOG_TRACE(
        "macro/state-repair",
        "authorized specialized macro-state transition: directive=#{0} "
        "source=[{1},{2}) originalBoundary={3}:{4} finalBoundary={5}:{6} "
        "replacementOffset={7} mutation={8} disposition={9} "
        "orderedObservers={10} incomparableObservers={11} reason={12}",
        transition.source.directive->id, transition.source.interval.begin,
        transition.source.interval.end,
        transition.originalBoundary.source.begin,
        transition.originalBoundary.source.end,
        transition.finalBoundary.source.begin,
        transition.finalBoundary.source.end,
        transition.finalReplacementOffset
            ? std::to_string(*transition.finalReplacementOffset)
            : std::string("none"),
        transition.mutation,
        macroStateTransitionDispositionName(transition.disposition),
        transition.relevantObservers.orderedObservers.size(),
        transition.relevantObservers.incomparableResults.size(),
        transition.reason);
  }
  return authorized;
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::ActiveDefinitionAtSourceOffset(StringRef macroName,
                                                        uint64_t offset) const {
  const RefoldModel::MacroDirective *active = nullptr;
  uint64_t activeEnd = 0;

  // Only directives controlling this macro can define its state here, so ask
  // the name index rather than scanning every directive in the unit.  The
  // bucket is in the same ascending-id order the ordered view uses, and the
  // winner below is chosen by explicit comparison, so this visits a subset in
  // the same relative order and selects the same directive.
  const auto bucket = plan_.namedMacroDirectivesByName.find(macroName);
  if (bucket == plan_.namedMacroDirectivesByName.end())
    return nullptr;

  for (uint32_t position : bucket->second) {
    const NamedMacroDirectiveRef &ref = plan_.namedMacroDirectives[position];
    const RefoldModel::MacroDirective &candidate = *ref.directive;
    std::optional<MacroStateSourceTransition> transition =
        MacroStateSourceTransitionFor(candidate);
    if (!transition || transition->interval.end > offset)
      continue;
    if (!active || transition->interval.end > activeEnd ||
        (transition->interval.end == activeEnd && candidate.id > active->id)) {
      active = &candidate;
      activeEnd = transition->interval.end;
    }
  }
  return active && active->subkind == "#define" ? active : nullptr;
}

bool MacroStateRepairContext::MacroStateDirectiveCanBeDelayedAfterEdit(
    const TextEdit &edit, const RefoldModel::MacroDirective &directive) const {
  if (PathIdentity().PathsEqual(directive.sitePath, tuPath_) &&
      !directive.ownerIncludeId) {
    std::optional<MacroDirectiveSourceInterval> directiveInterval =
        MacroDirectiveFullSourceInterval(directive);
    return directiveInterval && edit.start <= directiveInterval->begin &&
           directiveInterval->end <= edit.end;
  }

  const RefoldModel::IncludeItem *inc = OwningIncludeSiteInTU(directive);
  if (!inc)
    return false;
  if (inc->siteB < edit.start || edit.end < inc->siteE)
    return false;

  // If the replacement carries the include directive itself, the header
  // transition remains available through ordinary source order; hoisting the
  // header's macro-state directive into the TU would duplicate it.
  return !IncludeDirectiveAppearsAtLineStart(edit, *inc);
}

bool MacroStateRepairContext::MacroStateDefinitionAvailableBeforeSourceOffset(
    const RefoldModel::MacroDirective &definition, uint64_t offset) const {
  if (definition.subkind != "#define")
    return false;

  if (PathIdentity().PathsEqual(definition.sitePath, tuPath_) &&
      !definition.ownerIncludeId) {
    std::optional<MacroDirectiveSourceInterval> interval =
        MacroDirectiveFullSourceInterval(definition);
    return interval && interval->end <= offset;
  }

  const RefoldModel::IncludeItem *inc = OwningIncludeSiteInTU(definition);
  return inc && inc->siteE <= offset;
}

const RefoldModel::MacroInvocation *
MacroStateRepairContext::PhysicalRootInvocation(
    const RefoldModel::MacroInvocation &invocation) const {
  const uint64_t rootId = MacroTopology().GetRootMacroId(invocation.id);
  return MacroTopology().FindMacroInvocationById(rootId);
}

bool MacroStateRepairContext::PhysicalCallsiteIsMaterialized(
    const RefoldModel::MacroInvocation &invocation) const {
  if (!invocation.invFile || !invocation.invB || !invocation.invE ||
      *invocation.invE < *invocation.invB) {
    return false;
  }

  RefoldStructuralHunkDispatcher::MacroPatchStagingSlot slot =
      dispatcher_.PrepareMacroPatchStagingSlot(invocation);
  if (slot.existingPatch && !slot.existingIsCallsite)
    return true;

  if (PathIdentity().PathsEqual(*invocation.invFile, tuPath_)) {
    return FinalTUEditContainingInterval(*invocation.invB, *invocation.invE)
        .has_value();
  }

  const RefoldModel::IncludeItem *inc =
      OwningIncludeSiteForInvocationInTU(invocation);
  return inc && FinalTUEditContainingInterval(inc->siteB, inc->siteE);
}


bool MacroStateRepairContext::AuthorizeMaterializedDefinitionTransitions() {
  struct DefinitionMaterializationState {
    const RefoldModel::MacroDirective *definition = nullptr;
    bool sawPhysicalCallsite = false;
    bool allPhysicalCallsitesMaterialized = true;
  };

  std::map<uint64_t, DefinitionMaterializationState> states;
  std::set<std::pair<uint64_t, uint64_t>> processedDefinitionRootPairs;

  // A consumed definition is not authorized merely because its source line is
  // covered by an ordinary TU edit. Instead, collect every producer-linked
  // physical root that depends on that exact definition and require each root
  // to have an accepted materialized disposition. Folding nested expansion
  // nodes to the physical root prevents one callsite from being counted as
  // several independent proofs, while the pair key preserves distinct
  // definition dependencies at the same root.
  for (const RefoldModel::MacroInvocation &invocation :
       Model().GetMacroInvocations()) {
    const RefoldModel::MacroDirective *definition =
        ActiveDefinitionForInvocation(invocation);
    if (!definition || definition->subkind != "#define" ||
        !DefinitionDirectiveTouchedByTUEdit(*definition)) {
      continue;
    }

    const RefoldModel::MacroInvocation *root =
        PhysicalRootInvocation(invocation);
    if (!root) {
      TerminalSink().RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "macro/state-repair",
          llvm::formatv("producer-linked invocation #{0} of consumed "
                        "definition #{1} has no exact physical root",
                        invocation.id, definition->id)
              .str());
      return false;
    }

    if (!processedDefinitionRootPairs
             .insert(std::make_pair(definition->id, root->id))
             .second) {
      continue;
    }

    DefinitionMaterializationState &state = states[definition->id];
    state.definition = definition;
    state.sawPhysicalCallsite = true;
    state.allPhysicalCallsitesMaterialized &=
        PhysicalCallsiteIsMaterialized(*root);
  }

  for (const auto &entry : states) {
    const DefinitionMaterializationState &state = entry.second;
    if (!state.definition || !state.sawPhysicalCallsite ||
        !state.allPhysicalCallsitesMaterialized ||
        plan_.preservedDefinitionDirectiveIds.contains(entry.first)) {
      continue;
    }

    std::optional<MacroStateSourceTransition> source =
        MacroStateSourceTransitionFor(*state.definition);
    std::optional<size_t> editIndex;
    if (source) {
      editIndex = FinalTUEditContainingInterval(source->interval.begin,
                                                source->interval.end);
    }
    if (!source || !editIndex || *editIndex >= tuEdits_.size()) {
      TerminalSink().RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "macro-definition-materialization",
          "materialized definition transition has no exact containing final "
          "TU edit");
      return false;
    }

    const std::string reason =
        llvm::formatv("every producer-linked physical callsite of consumed "
                      "definition #{0} has an accepted materialized "
                      "disposition",
                      state.definition->id)
            .str();
    std::optional<ProvenMacroStateSourceTransition> transition =
        ProveMacroStateSourceTransition(
            *state.definition, StateMutationKind::Materialized,
            MacroStateTransitionDisposition::Materialized,
            FinalTUStateBoundary(tuEdits_[*editIndex].end), std::nullopt,
            SuffixStabilityWitnessKind::OwnerMaterialization,
            "macro-definition-materialization", reason,
            // The materialized physical roots above are the concrete
            // observation facts. Once all of them are removed, a suffix
            // observer is not required merely to manufacture authority for the
            // source transition. Any observer that remains is still recorded
            // by the state gateway and the complete observer census below.
            /*RequireKnownObserver=*/false);
    if (!transition || !AuthorizeMacroStateSourceTransition(
                           tuEdits_[*editIndex], *transition)) {
      return false;
    }
    PromoteToSpecializedMacroStateRepairCarrier(tuEdits_[*editIndex]);
  }

  return !TerminalSink().HasRequest();
}

std::optional<size_t>
MacroStateRepairContext::FirstReplacementObservationOffset(
    const TextEdit &edit, const RefoldModel::MacroDirective &definition,
    StringRef macroName) const {
  return MacroStateProof().FirstMacroStateObservationOffsetInText(
      definition, macroName, StringRef(edit.text),
      tuBytes_.drop_front(edit.end));
}

bool MacroStateRepairContext::ReplacementObservesPreservedDefinition(
    const TextEdit &edit, const RefoldModel::MacroDirective &definition,
    StringRef macroName) const {
  return FirstReplacementObservationOffset(edit, definition, macroName)
      .has_value();
}

std::optional<size_t>
MacroStateRepairContext::ReplacementLineStartBeforeObservation(
    StringRef replacement, size_t observationOffset) const {
  if (observationOffset > replacement.size())
    return std::nullopt;

  for (size_t i = observationOffset; i > 0; --i) {
    const size_t newline = i - 1;
    if (replacement[newline] != '\n')
      continue;
    if (stringutils::isLineSplice(replacement, newline))
      return std::nullopt;
    return newline + 1;
  }
  return std::nullopt;
}

std::optional<size_t>
MacroStateRepairContext::ReplacementLineStartAfterFinalObservation(
    const TextEdit &edit, const RefoldModel::MacroDirective &definition,
    StringRef macroName, size_t firstObservationOffset) const {
  StringRef Replacement(edit.text);
  if (firstObservationOffset >= Replacement.size())
    return std::nullopt;

  for (size_t i = firstObservationOffset + 1; i < Replacement.size(); ++i) {
    const size_t newline = i - 1;
    if (Replacement[newline] != '\n')
      continue;
    if (stringutils::isLineSplice(Replacement, newline))
      continue;

    // Re-run the ordinary observation proof on the replacement suffix that
    // would appear after the inserted directive.  If that suffix has no
    // observing token, every replacement byte that must remain in B's
    // pre-definition macro state stays before the insertion point.
    TextEdit suffixEdit = edit;
    suffixEdit.text = Replacement.drop_front(i).str();
    if (!FirstReplacementObservationOffset(suffixEdit, definition, macroName))
      return i;
  }

  return std::nullopt;
}

bool MacroStateRepairContext::ReplacementSuffixBoundaryAllowsDirectiveLine(
    const TextEdit &edit) const {
  if (edit.end > tuBytes_.size())
    return false;

  StringRef Text(edit.text);
  if (!Text.empty()) {
    if (Text.back() == '\n') {
      if (stringutils::isLineSplice(Text, Text.size() - 1))
        return false;
    } else if (Text.back() == '\\') {
      // Adding the newline needed to start a directive would form a line splice
      // with the replacement payload. Do not preserve through that boundary
      // without a stronger proof.
      return false;
    }
  }

  if (Text.empty() || stringutils::isWs(Text.back()))
    return true;
  if (edit.end >= tuBytes_.size() || stringutils::isWs(tuBytes_[edit.end]))
    return true;

  std::optional<RefoldLexBoundaryToken> leftTok =
      refoldLastLexToken(Text, LexLang());
  std::optional<RefoldLexBoundaryToken> rightTok =
      refoldFirstLexToken(tuBytes_.drop_front(edit.end), LexLang());
  if (!leftTok || !rightTok)
    return true;

  return !refoldNeedsLexicalSeparator(*leftTok, *rightTok, LexLang());
}

bool MacroStateRepairContext::EditHasMacroPatchSurfaceInBMacroState(
    const TextEdit &edit) const {
  for (const auto &carrier : edit.acceptedResults) {
    if (!carrier || carrier->kind != AcceptedResultCandidateKind::MacroPatch)
      continue;

    const ProofSummary &summary = carrier->proofSummary;
    if ((summary.theoremClass == TheoremProofClass::OwnerRealizationProof ||
         summary.theoremClass == TheoremProofClass::MixedOwnerTilingProof) &&
        summary.inventory.currentPath ==
            AcceptedPathKind::MacroWholeCoverRealization &&
        summary.realizationMode == RealizationMode::RealizeEditedSurface &&
        summary.surfaceDisposition ==
            SurfaceDisposition::RealizeWholeCoverMacros)
      return true;
  }
  return false;
}

bool MacroStateRepairContext::
    DirectCalleePatchRequiresDefinitionBeforeReplacement(
        const TextEdit &edit, const RefoldModel::MacroDirective &definition,
        StringRef macroName) const {
  if (definition.subkind != "#define")
    return false;
  if (!ReplacementObservesPreservedDefinition(edit, definition, macroName))
    return false;

  for (const auto &carrier : edit.acceptedResults) {
    if (!carrier || carrier->kind != AcceptedResultCandidateKind::MacroPatch)
      continue;

    const ProofSummary &summary = carrier->proofSummary;
    if (summary.inventory.currentPath !=
        AcceptedPathKind::MacroDirectCalleeSubstitution)
      continue;

    // A direct-callee substitution is not a B-surface realization whose
    // observing macro definitions should be delayed past the replacement.  Its
    // proof says that the emitted invocation must be preprocessed under the
    // ordinary source macro state available at the original callsite.  Moving
    // an observed gap definition after the rewritten call would invalidate the
    // alternate-callee replay witness, as in ADD(...) -> SUB(...).
    return true;
  }

  return false;
}

/// A TU include closure realizes an unresolved hunk by keeping the original
/// `#include` line, and any source-neutral zero-token material around it,
/// inside the replacement alongside the B payload.  Its replacement text is
/// therefore a mixture of two macro states: the B payload is already fully
/// expanded, while the preserved source must still be preprocessed exactly as
/// it was.
///
/// No fact currently partitions a replacement into its B-derived and
/// preserved-source bytes, so this reports only that such a mixture exists.
/// Callers that would rewrite the macro state of a whole replacement must treat
/// that as missing evidence rather than as permission.
bool MacroStateRepairContext::EditClosesOverPreservedTUSource(
    const TextEdit &edit) const {
  for (const auto &carrier : edit.acceptedResults) {
    if (!carrier)
      continue;
    if (carrier->proofSummary.inventory.currentPath ==
        AcceptedPathKind::TUIncludeClosureEdit)
      return true;
  }
  return false;
}

OwnerStateBoundary MacroStateRepairContext::MacroDirectiveSuffixBoundary(
    const RefoldModel::MacroDirective &directive) const {
  return OwnerStateBoundary::FromSource(
      OwnerSourceRange::From(directive.sitePath, directive.siteB,
                             directive.siteE, directive.ownerIncludeId));
}

StateTransitionProof MacroStateRepairContext::CheckMacroStateWithWitness(
    const OwnerStateBoundary &boundary, StateMutationKind mutation,
    SuffixStabilityWitness witness, StringRef stage, StringRef detail,
    bool requireKnownObserver) const {
  return OwnerStateProof().CheckStateTransitionAcrossEditBoundary(
      boundary, OwnerStateComponent::MacroState, mutation, std::move(witness),
      stage, detail, requireKnownObserver);
}

StateTransitionProof MacroStateRepairContext::CheckMacroStateRepaired(
    const RefoldModel::MacroDirective &directive, StateMutationKind mutation,
    StringRef stage, StringRef detail, bool requireKnownObserver) const {
  const OwnerStateBoundary boundary = MacroDirectiveSuffixBoundary(directive);
  return CheckMacroStateWithWitness(
      boundary, mutation,
      OwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::StateRepair,
          OwnerStateComponent::MacroState, boundary, detail),
      stage, detail, requireKnownObserver);
}

StateTransitionProof MacroStateRepairContext::CheckMacroStateTerminal(
    const RefoldModel::MacroDirective &directive, StateMutationKind mutation,
    StringRef stage, StringRef detail, bool requireKnownObserver,
    std::optional<uint64_t> failingOwnerId) const {
  const OwnerStateBoundary boundary = MacroDirectiveSuffixBoundary(directive);
  SuffixStabilityWitness witness = OwnerStateProof().BuildStateTransitionWitness(
      SuffixStabilityWitnessKind::TerminalStateFailure,
      OwnerStateComponent::MacroState, boundary, detail);

  // Prefer the caller's region over the directive's own include instance.  The
  // gateway fills the boundary's include id in when nothing more specific
  // arrives, and the directive's owner is the header the `#define` sits in --
  // which is not the region whose realization just failed.
  if (failingOwnerId && witness.terminalFailure)
    witness.terminalFailure->failure.context.ownerId = failingOwnerId;

  return CheckMacroStateWithWitness(boundary, mutation, std::move(witness),
                                    stage, detail, requireKnownObserver);
}

StateMutationKind
MacroStateRepairContext::MutationForMacroStatePreservationPlacement(
    MacroStatePreservationPlacement placement) const {
  switch (placement) {
  case MacroStatePreservationPlacement::BeforeReplacement:
  case MacroStatePreservationPlacement::InsideReplacement:
  case MacroStatePreservationPlacement::AfterReplacement:
    return StateMutationKind::Replayed;
  case MacroStatePreservationPlacement::AdvancedBeforeReplacement:
    return StateMutationKind::MovedEarlier;
  }
  llvm_unreachable("invalid macro-state preservation placement");
}

StringRef MacroStateRepairContext::MacroStatePreservationPlacementName(
    MacroStatePreservationPlacement placement) const {
  switch (placement) {
  case MacroStatePreservationPlacement::BeforeReplacement:
    return "before-replacement";
  case MacroStatePreservationPlacement::InsideReplacement:
    return "inside-replacement";
  case MacroStatePreservationPlacement::AfterReplacement:
    return "after-replacement";
  case MacroStatePreservationPlacement::AdvancedBeforeReplacement:
    return "advanced-before-replacement";
  }
  return "unknown";
}

void MacroStateRepairContext::AttachConservativeTUCarrier(TextEdit &edit) {
  TextEditAssembler().AttachAcceptedResultCarrier(
      edit, ProofLattice()
                .AcceptedCandidateBuilder()
                .BuildAcceptedSpecializedTUTextEditCandidate(
                    AcceptedPathKind::TUByteSpanConservativeEdit, edit.start,
                    edit.end, StringRef(edit.text)));
}

void MacroStateRepairContext::PromoteToSpecializedMacroStateRepairCarrier(
    TextEdit &edit) {
  // A repaired source surface is no longer justified by the ordinary token
  // hunk theorem.  Remove only carriers whose canonical owner-realization
  // evidence is TUByteSpan; independently discharged macro/include carriers
  // remain attached when the repair composes with them.
  llvm::erase_if(
      edit.acceptedResults,
      [&](const std::shared_ptr<const AcceptedResultCandidate> &candidate) {
        if (!candidate ||
            candidate->kind != AcceptedResultCandidateKind::TUTextEdit)
          return false;
        if (AcceptedResultIsOrdinaryDirectTUCarrier(*candidate))
          return true;
        return AcceptedResultIsSpecializedTUCarrier(
                   *candidate,
                   AcceptedPathKind::TUByteSpanConservativeEdit) &&
               (candidate->begin != edit.start || candidate->end != edit.end ||
                !candidate->hasPayloadPreview ||
                candidate->payloadPreview != edit.text);
      });

  // Direct-hunk coordinates are implementation provenance for the ordinary
  // carrier.  Once macro-state proof changes or authorizes the source surface,
  // retaining them would let the final assembler mistake this specialized edit
  // for a raw direct span.
  edit.isDirectTUHunkEdit = false;
  edit.directTUHunkIndex.reset();
  edit.directTUHunkAStart.reset();
  edit.directTUHunkAEnd.reset();
  edit.directTUHunkBStart.reset();
  edit.directTUHunkBEnd.reset();
  edit.directTURawStart.reset();
  edit.directTURawEnd.reset();
  edit.directTUFinalStart.reset();
  edit.directTUFinalEnd.reset();

  const bool alreadySpecialized = llvm::any_of(
      edit.acceptedResults,
      [&](const std::shared_ptr<const AcceptedResultCandidate> &candidate) {
        if (!candidate ||
            candidate->kind != AcceptedResultCandidateKind::TUTextEdit ||
            candidate->begin != edit.start || candidate->end != edit.end)
          return false;
        return AcceptedResultIsSpecializedTUCarrier(
            *candidate, AcceptedPathKind::TUByteSpanConservativeEdit);
      });
  if (!alreadySpecialized)
    AttachConservativeTUCarrier(edit);
}

void MacroStateRepairContext::RestageConservativeTUEdit(
    TextEdit &edit, uint64_t start, uint64_t end, StringRef replacement,
    ArrayRef<ProvenMacroStateSourceTransition> repairedTransitions) {
  ResyncOutcome resync = TextEditAssembler().ApplyResyncOrPend(
      tuBytes_, start, end, replacement, tuPath_);
  edit.start = start;
  edit.end = end;
  edit.text = std::move(resync.text);
  edit.pending = std::move(resync.pending);
  edit.lineControlPruneCandidates =
      std::move(resync.lineControlPruneCandidates);

  // Restaging replaces the original direct-hunk carrier with a specialized
  // macro-state repair surface. Preserve independently discharged specialized
  // capabilities that remain inside the widened edit, discard capabilities
  // that no longer fit, and add only the exact transitions proved by this
  // repair. No capability is inferred merely from source containment.
  llvm::erase_if(
      edit.protectedSourceAuthorizations,
      [&](const ProtectedSourceEditAuthorization &authorization) {
        return authorization.begin < start || end < authorization.end;
      });
  for (const ProvenMacroStateSourceTransition &transition :
       repairedTransitions) {
    if (!AuthorizeMacroStateSourceTransition(edit, transition))
      return;
  }
  PromoteToSpecializedMacroStateRepairCarrier(edit);
}

std::optional<uint64_t>
MacroStateRepairContext::DelayedTransitionBoundaryAfterEdit(
    size_t editIndex, const RefoldModel::MacroDirective &definition,
    StringRef macroName) const {
  if (editIndex >= tuEdits_.size())
    return std::nullopt;
  const TextEdit &edit = tuEdits_[editIndex];
  if (edit.end > tuBytes_.size())
    return std::nullopt;

  const size_t editEnd = static_cast<size_t>(edit.end);
  size_t lineEnd = stringutils::lineEndOffset(tuBytes_, editEnd);

  // Lexical separability alone is not enough for natural directive placement.
  // Splitting `int x = M + 2;` as payload / directive / `;` preserves tokens
  // but moves a directive into a physical declaration.  Only use the
  // edit/suffix boundary directly when it is already at physical line end;
  // otherwise try to absorb the neutral rest of that source line.
  if (editEnd == lineEnd && ReplacementSuffixBoundaryAllowsDirectiveLine(edit))
    return edit.end;

  if (lineEnd <= editEnd)
    return std::nullopt;
  if (lineEnd < tuBytes_.size()) {
    if (stringutils::isLineSplice(tuBytes_, lineEnd))
      return std::nullopt;
    ++lineEnd;
  }

  if (SourceRangeOverlapsFinalTUEditExcept(edit.end, lineEnd, editIndex))
    return std::nullopt;

  StringRef carriedSuffix = tuBytes_.slice(edit.end, lineEnd);
  if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
          definition, macroName, carriedSuffix, tuBytes_.drop_front(lineEnd)))
    return std::nullopt;

  TextEdit boundaryEdit = edit;
  boundaryEdit.end = lineEnd;
  boundaryEdit.text.append(carriedSuffix.begin(), carriedSuffix.end());
  if (!ReplacementSuffixBoundaryAllowsDirectiveLine(boundaryEdit))
    return std::nullopt;

  return static_cast<uint64_t>(lineEnd);
}

void MacroStateRepairContext::
    AdvancePreservedUndefsBeforeObservedReplacements() {
  SmallVector<size_t, 16> editOrder;
  editOrder.reserve(tuEdits_.size());
  for (size_t editIndex = 0; editIndex < tuEdits_.size(); ++editIndex)
    editOrder.push_back(editIndex);
  llvm::sort(editOrder, [&](size_t lhs, size_t rhs) {
    if (tuEdits_[lhs].start != tuEdits_[rhs].start)
      return tuEdits_[lhs].start < tuEdits_[rhs].start;
    return lhs < rhs;
  });

  DenseSet<uint64_t> advancedDirectiveIds;
  size_t advancedCount = 0;

  for (size_t editIndex : editOrder) {
    if (editIndex >= tuEdits_.size())
      continue;
    TextEdit &edit = tuEdits_[editIndex];
    if (edit.start > edit.end || edit.end > tuBytes_.size())
      continue;

    const size_t editStart = static_cast<size_t>(edit.start);
    const size_t lineStart = stringutils::lineStartOffset(tuBytes_, editStart);
    if (lineStart >= editStart)
      continue;
    if (lineStart > 0 && stringutils::isLineSplice(tuBytes_, lineStart - 1))
      continue;
    if (SourceRangeOverlapsFinalTUEditExcept(lineStart, edit.start, editIndex))
      continue;

    StringRef crossedPrefix = tuBytes_.slice(lineStart, edit.start);
    StringRef ReplacementText(edit.text);

    for (const NamedMacroDirectiveRef &undefRef : plan_.namedMacroDirectives) {
      const RefoldModel::MacroDirective &undefDirective = *undefRef.directive;
      if (undefDirective.subkind != "#undef")
        continue;
      if (advancedDirectiveIds.contains(undefDirective.id))
        continue;

      std::optional<MacroStateSourceTransition> undefTransition =
          MacroStateSourceTransitionFor(undefDirective);
      if (!undefTransition)
        continue;

      // This pass only advances a preserved future transition. If the
      // transition begins inside the current edit, it is a consumed-#undef case
      // handled by the consumed-transition repair instead.
      if (undefTransition->interval.begin < edit.end)
        continue;

      const RefoldModel::MacroDirective *previousDefinition =
          ActiveDefinitionAtSourceOffset(undefRef.name, lineStart);
      if (!previousDefinition)
        continue;

      std::optional<size_t> firstObservationOffset =
          FirstReplacementObservationOffset(edit, *previousDefinition,
                                            undefRef.name);
      if (!firstObservationOffset)
        continue;

      if (SourceRangeOverlapsFinalTUEditExcept(
              lineStart, undefTransition->interval.end, editIndex))
        continue;

      StringRef replacementPrefix =
          ReplacementText.take_front(*firstObservationOffset);
      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *previousDefinition, undefRef.name, replacementPrefix,
              ReplacementText.drop_front(*firstObservationOffset)))
        continue;

      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *previousDefinition, undefRef.name, crossedPrefix,
              ReplacementText))
        continue;

      StringRef carriedSuffix =
          tuBytes_.slice(edit.end, undefTransition->interval.begin);
      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *previousDefinition, undefRef.name, carriedSuffix,
              tuBytes_.drop_front(undefTransition->interval.begin)))
        continue;

      std::string replacement;
      replacement.reserve(undefTransition->text.size() + crossedPrefix.size() +
                          ReplacementText.size() + carriedSuffix.size());
      replacement += undefTransition->text;
      replacement.append(crossedPrefix.begin(), crossedPrefix.end());
      replacement.append(ReplacementText.begin(), ReplacementText.end());
      replacement.append(carriedSuffix.begin(), carriedSuffix.end());

      const uint64_t oldStart = edit.start;
      const uint64_t oldEnd = edit.end;
      const std::string reason =
          llvm::formatv("advanced preserved #undef directive #{0} for macro "
                        "'{1}' before observing replacement [{2},{3})",
                        undefDirective.id, undefRef.name, oldStart, oldEnd)
              .str();
      std::optional<ProvenMacroStateSourceTransition> provedTransition =
          ProveMacroStateSourceTransition(
              undefDirective, StateMutationKind::MovedEarlier,
              MacroStateTransitionDisposition::MovedEarlier,
              FinalTUStateBoundary(lineStart), size_t{0},
              SuffixStabilityWitnessKind::StateRepair,
              "macro-undef-liveness", reason);
      if (!provedTransition)
        return;
      SmallVector<ProvenMacroStateSourceTransition, 1> repairedTransitions;
      repairedTransitions.push_back(std::move(*provedTransition));
      RestageConservativeTUEdit(edit, lineStart,
                                undefTransition->interval.end, replacement,
                                repairedTransitions);

      advancedDirectiveIds.insert(undefDirective.id);
      ++advancedCount;
      REFOLD_LOG_WARN("macro/liveness",
                      "advancing preserved #undef before observed replacement: "
                      "macro='{0}' undefDirective=#{1} priorDefine=#{2} "
                      "edit=[{3},{4}) widened=[{5},{6})",
                      undefRef.name, undefDirective.id, previousDefinition->id,
                      oldStart, oldEnd, edit.start, edit.end);
      break;
    }
  }

  if (advancedCount != 0) {
    REFOLD_LOG_INFO(
        "macro/liveness",
        "advanced {0} preserved #undef directive(s) before "
        "replacement payloads to keep edited tokens in B macro state",
        advancedCount);
  }
}

std::optional<MacroStatePreservationPlacement>
MacroStateRepairContext::TryAdvanceConsumedUndefBeforeObservedReplacement(
    size_t editIndex, const RefoldModel::MacroDirective &undefDirective,
    StringRef macroName, const RefoldModel::MacroDirective &previousDefinition,
    size_t firstObservationOffset) {
  if (editIndex >= tuEdits_.size())
    return std::nullopt;

  TextEdit &edit = tuEdits_[editIndex];
  if (edit.start == 0 || edit.start > tuBytes_.size())
    return std::nullopt;

  const size_t editStart = static_cast<size_t>(edit.start);
  const size_t lineStart = stringutils::lineStartOffset(tuBytes_, editStart);
  if (lineStart >= editStart)
    return std::nullopt;

  if (lineStart > 0 && stringutils::isLineSplice(tuBytes_, lineStart - 1))
    return std::nullopt;

  if (!MacroStateDefinitionAvailableBeforeSourceOffset(previousDefinition,
                                                       lineStart))
    return std::nullopt;

  if (SourceRangeOverlapsFinalTUEditExcept(lineStart, edit.start, editIndex))
    return std::nullopt;

  StringRef ReplacementText(edit.text);
  if (firstObservationOffset > ReplacementText.size())
    return std::nullopt;

  StringRef replacementPrefix =
      ReplacementText.take_front(firstObservationOffset);
  if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
          previousDefinition, macroName, replacementPrefix,
          ReplacementText.drop_front(firstObservationOffset)))
    return std::nullopt;

  StringRef crossedPrefix = tuBytes_.slice(lineStart, edit.start);
  if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
          previousDefinition, macroName, crossedPrefix, ReplacementText))
    return std::nullopt;

  std::string replacement;
  const std::string directiveText =
      DirectiveTextForPreservation(undefDirective);
  replacement.reserve(directiveText.size() + crossedPrefix.size() +
                      ReplacementText.size());
  replacement += directiveText;
  replacement.append(crossedPrefix.begin(), crossedPrefix.end());
  replacement.append(ReplacementText.begin(), ReplacementText.end());

  std::optional<MacroStateSourceTransition> undefTransition =
      MacroStateSourceTransitionFor(undefDirective);
  if (!undefTransition)
    return std::nullopt;

  const uint64_t oldStart = edit.start;
  const std::string reason =
      llvm::formatv("advanced consumed #undef directive #{0} for macro '{1}' "
                    "before replacement observing prior definition #{2}",
                    undefDirective.id, macroName, previousDefinition.id)
          .str();
  std::optional<ProvenMacroStateSourceTransition> provedTransition =
      ProveMacroStateSourceTransition(
          undefDirective, StateMutationKind::MovedEarlier,
          MacroStateTransitionDisposition::MovedEarlier,
          FinalTUStateBoundary(lineStart), size_t{0},
          SuffixStabilityWitnessKind::StateRepair, "macro-undef-liveness",
          reason);
  if (!provedTransition)
    return std::nullopt;
  SmallVector<ProvenMacroStateSourceTransition, 1> repairedTransitions;
  repairedTransitions.push_back(std::move(*provedTransition));
  RestageConservativeTUEdit(edit, lineStart, edit.end, replacement,
                            repairedTransitions);

  REFOLD_LOG_WARN("macro/liveness",
                  "advancing consumed #undef before observed replacement: "
                  "macro='{0}' undefDirective=#{1} priorDefine=#{2} "
                  "edit=[{3},{4}) widenedStart={5}",
                  macroName, undefDirective.id, previousDefinition.id, oldStart,
                  edit.end, edit.start);

  return MacroStatePreservationPlacement::AdvancedBeforeReplacement;
}

bool MacroStateRepairContext::TryWidenEditToDelayedMacroStateBoundary(
    size_t editIndex, const RefoldModel::MacroDirective &definition,
    StringRef macroName) {
  if (editIndex >= tuEdits_.size())
    return false;

  TextEdit &edit = tuEdits_[editIndex];
  std::optional<uint64_t> boundary =
      DelayedTransitionBoundaryAfterEdit(editIndex, definition, macroName);
  if (!boundary)
    return false;
  if (*boundary == edit.end)
    return true;
  if (*boundary < edit.end || *boundary > tuBytes_.size())
    return false;

  std::string replacement = edit.text;
  replacement.append(tuBytes_.begin() + edit.end, tuBytes_.begin() + *boundary);

  const uint64_t oldEnd = edit.end;
  RestageConservativeTUEdit(edit, edit.start, *boundary, replacement);

  REFOLD_LOG_TRACE("macro/liveness",
                   "widened TU edit to delayed macro-state boundary: "
                   "macro='{0}' definitionDirective=#{1} oldEnd={2} newEnd={3}",
                   macroName, definition.id, oldEnd, edit.end);
  return true;
}

std::optional<MacroStatePreservationPlacement>
MacroStateRepairContext::TryQueueMacroStateDirectivePreservation(
    size_t editIndex, const RefoldModel::MacroDirective &directive,
    StringRef macroName,
    const RefoldModel::MacroDirective *observedDefinition) {
  if (editIndex >= tuEdits_.size())
    return std::nullopt;

  TextEdit &edit = tuEdits_[editIndex];

  const RefoldModel::MacroDirective *definitionObservedByReplacement =
      observedDefinition
          ? observedDefinition
          : (directive.subkind == "#define" ? &directive : nullptr);
  const std::optional<size_t> firstObservationOffset =
      definitionObservedByReplacement
          ? FirstReplacementObservationOffset(
                edit, *definitionObservedByReplacement, macroName)
          : TokenTextAnalysis().FirstRawIdentifierObservationOffsetInText(
                macroName, StringRef(edit.text));
  const bool replacementObservesDefinition = firstObservationOffset.has_value();

  const bool editStartsAtPhysicalBOL =
      edit.start == 0 || tuBytes_[edit.start - 1] == '\n';
  const bool beforePlacementWouldExposeDefine =
      directive.subkind == "#define" && replacementObservesDefinition;
  if (editStartsAtPhysicalBOL && !beforePlacementWouldExposeDefine) {
    macroStatePreservationsByEdit_[editIndex].push_back(MacroStatePreservation{
        &directive, MacroStatePreservationPlacement::BeforeReplacement});
    return MacroStatePreservationPlacement::BeforeReplacement;
  }

  if (!MacroStateDirectiveCanBeDelayedAfterEdit(edit, directive))
    return std::nullopt;

  if (directive.subkind == "#undef" && replacementObservesDefinition) {
    if (!firstObservationOffset || !definitionObservedByReplacement)
      return std::nullopt;
    std::optional<size_t> insertionOffset =
        ReplacementLineStartBeforeObservation(StringRef(edit.text),
                                              *firstObservationOffset);
    if (insertionOffset) {
      macroStatePreservationsByEdit_[editIndex].push_back(
          MacroStatePreservation{
              &directive, MacroStatePreservationPlacement::InsideReplacement,
              *insertionOffset});
      return MacroStatePreservationPlacement::InsideReplacement;
    }

    return TryAdvanceConsumedUndefBeforeObservedReplacement(
        editIndex, directive, macroName, *definitionObservedByReplacement,
        *firstObservationOffset);
  }

  if (directive.subkind == "#define" && replacementObservesDefinition &&
      definitionObservedByReplacement) {
    if (!firstObservationOffset)
      return std::nullopt;
    std::optional<size_t> insertionOffset =
        ReplacementLineStartAfterFinalObservation(
            edit, *definitionObservedByReplacement, macroName,
            *firstObservationOffset);
    if (insertionOffset) {
      macroStatePreservationsByEdit_[editIndex].push_back(
          MacroStatePreservation{
              &directive, MacroStatePreservationPlacement::InsideReplacement,
              *insertionOffset});
      return MacroStatePreservationPlacement::InsideReplacement;
    }
  }

  if (directive.subkind == "#undef" && !replacementObservesDefinition &&
      definitionObservedByReplacement) {
    (void)TryWidenEditToDelayedMacroStateBoundary(
        editIndex, *definitionObservedByReplacement, macroName);
  }

  if (!ReplacementSuffixBoundaryAllowsDirectiveLine(edit))
    return std::nullopt;

  macroStatePreservationsByEdit_[editIndex].push_back(MacroStatePreservation{
      &directive, MacroStatePreservationPlacement::AfterReplacement});
  return MacroStatePreservationPlacement::AfterReplacement;
}

// Insert a synthetic #undef at the start of a widened replacement line when
// the preserved source before the edit intentionally crosses a definition, but
// the replacement text must observe B's undefined macro state.  This is a
// conservative token-level repair: it does not attempt to restore the
// definition later, so suffix text that still needs the definition fails
// closed.
void MacroStateRepairContext::SynthesizeUndefBeforeObservedGapDefinitions() {
  SmallVector<size_t, 16> editOrder;
  editOrder.reserve(tuEdits_.size());
  for (size_t editIndex = 0; editIndex < tuEdits_.size(); ++editIndex)
    editOrder.push_back(editIndex);
  llvm::sort(editOrder, [&](size_t lhs, size_t rhs) {
    if (tuEdits_[lhs].start != tuEdits_[rhs].start)
      return tuEdits_[lhs].start < tuEdits_[rhs].start;
    return lhs < rhs;
  });

  size_t synthesizedCount = 0;
  for (size_t editIndex : editOrder) {
    if (editIndex >= tuEdits_.size())
      continue;
    TextEdit &edit = tuEdits_[editIndex];
    if (edit.start > edit.end || edit.end > tuBytes_.size())
      continue;

    // This partition rewrites the macro state seen by the entire replacement.
    // That is justified only for a B-derived payload: B is already fully
    // expanded, so a live definition named there would be a spurious
    // re-expansion that the synthetic #undef removes.  When the replacement
    // also carries preserved TU source, an observation may instead be an
    // expansion the preserved source requires, and nothing records which
    // replacement bytes are which.  Refuse rather than assume: undefining the
    // macro would otherwise silently change how the preserved source
    // preprocesses.
    if (EditClosesOverPreservedTUSource(edit))
      continue;

    const size_t editStart = static_cast<size_t>(edit.start);
    const size_t lineStart = stringutils::lineStartOffset(tuBytes_, editStart);
    if (lineStart > editStart)
      continue;
    if (lineStart > 0 && stringutils::isLineSplice(tuBytes_, lineStart - 1))
      continue;
    if (SourceRangeOverlapsFinalTUEditExcept(lineStart, edit.start, editIndex))
      continue;

    StringRef sameLinePrefix = tuBytes_.slice(lineStart, edit.start);
    StringRef ReplacementText(edit.text);
    StringRef untouchedSuffix = tuBytes_.drop_front(edit.end);

    SmallVector<SyntheticUndefCandidate, 4> candidates;
    for (const NamedMacroDirectiveRef &ref : plan_.namedMacroDirectives) {
      const RefoldModel::MacroDirective &definitionLocal = *ref.directive;
      if (definitionLocal.subkind != "#define")
        continue;
      if (plan_.syntheticUndefPartitionedDefinitionIds.contains(
              definitionLocal.id))
        continue;

      std::optional<MacroStateSourceTransition> transition =
          MacroStateSourceTransitionFor(definitionLocal);
      if (!transition)
        continue;
      if (transition->interval.end > lineStart)
        continue;
      if (ActiveDefinitionAtSourceOffset(ref.name, lineStart) !=
          &definitionLocal)
        continue;

      std::optional<size_t> firstObservationOffset =
          FirstReplacementObservationOffset(edit, definitionLocal, ref.name);
      if (!firstObservationOffset)
        continue;

      // This synthetic partition is a first-order token repair: it is meant for
      // replacement payloads such as `M()` that would otherwise be re-expanded
      // by a live definition.  If the first apparent observation is on, or
      // after, a preprocessing directive in the replacement text, the situation
      // is no longer a plain token observation.  The directive may select a
      // zero-token arm, an inactive arm, or a branch whose conditional state is
      // itself part of the owner proof.  Inserting an #undef before that
      // directive would be a gratuitous state mutation in the common
      // zero-token-gap case, and a stronger conditional-state tiling proof is
      // needed for the remaining cases.
      if (TokenTextAnalysis().TextContainsDirectiveLine(
              ReplacementText.take_front(*firstObservationOffset + 1)))
        continue;

      // If the original same-line prefix needed the definition, placing a
      // synthetic #undef before the line would change preserved source before
      // the replacement.
      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              definitionLocal, ref.name, sameLinePrefix, ReplacementText))
        continue;

      // Use this partition only when the crossed pre-edit region really is a
      // macro-state barrier/observer.  If it is neutral, the existing carry
      // proof can move the definition after the replacement without adding a
      // synthetic transition.
      if (!MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              definitionLocal, ref.name,
              tuBytes_.slice(transition->interval.end, edit.start),
              ReplacementText))
        continue;

      // This minimal partition does not restore the definition after the
      // replacement.  Do not synthesize it when later preserved source would
      // observe the old definition; that requires an explicit restore tiling.
      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              definitionLocal, ref.name, untouchedSuffix, StringRef()))
        continue;

      candidates.push_back(SyntheticUndefCandidate{
          &definitionLocal, transition->interval, ref.name.str()});
    }

    if (candidates.empty())
      continue;

    llvm::sort(candidates, [](const SyntheticUndefCandidate &lhs,
                              const SyntheticUndefCandidate &rhs) {
      if (lhs.interval.begin != rhs.interval.begin)
        return lhs.interval.begin < rhs.interval.begin;
      return lhs.definition->id < rhs.definition->id;
    });

    std::string undefPrefix;
    for (const SyntheticUndefCandidate &candidate : candidates) {
      undefPrefix += "#undef ";
      undefPrefix += candidate.name;
      undefPrefix.push_back('\n');
    }

    std::string replacement;
    replacement.reserve(undefPrefix.size() + sameLinePrefix.size() +
                        ReplacementText.size());
    replacement += undefPrefix;
    replacement.append(sameLinePrefix.begin(), sameLinePrefix.end());
    replacement.append(ReplacementText.begin(), ReplacementText.end());

    const uint64_t oldStart = edit.start;
    const uint64_t oldEnd = edit.end;
    RestageConservativeTUEdit(edit, lineStart, oldEnd, replacement);

    for (const SyntheticUndefCandidate &candidate : candidates) {
      plan_.syntheticUndefPartitionedDefinitionIds.insert(
          candidate.definition->id);
      (void)CheckMacroStateRepaired(
          *candidate.definition, StateMutationKind::MovedEarlier,
          "macro-synthetic-undef-partition",
          llvm::formatv("synthesized #undef for active definition #{0} of "
                        "macro '{1}' before observing TU replacement [{2},{3})",
                        candidate.definition->id, candidate.name, oldStart,
                        oldEnd)
              .str());
      ++synthesizedCount;
    }

    REFOLD_LOG_WARN("macro/liveness",
                    "synthesizing local #undef partition before observed "
                    "replacement: defs={0} edit=[{1},{2}) widened=[{3},{4})",
                    candidates.size(), oldStart, oldEnd, edit.start, edit.end);
  }

  if (synthesizedCount != 0)
    REFOLD_LOG_INFO(
        "macro/liveness",
        "synthesized {0} local #undef partition(s) before observing "
        "replacement payloads",
        synthesizedCount);
}

void MacroStateRepairContext::CarryObservedGapDefinitionsAfterReplacements() {
  SmallVector<size_t, 16> editOrder;
  editOrder.reserve(tuEdits_.size());
  for (size_t editIndex = 0; editIndex < tuEdits_.size(); ++editIndex)
    editOrder.push_back(editIndex);
  llvm::sort(editOrder, [&](size_t lhs, size_t rhs) {
    if (tuEdits_[lhs].start != tuEdits_[rhs].start)
      return tuEdits_[lhs].start < tuEdits_[rhs].start;
    return lhs < rhs;
  });

  DenseSet<uint64_t> carriedDirectiveIds;
  size_t carriedCount = 0;

  for (size_t editIndex : editOrder) {
    if (editIndex >= tuEdits_.size())
      continue;
    TextEdit &edit = tuEdits_[editIndex];
    if (edit.start > edit.end || edit.end > tuBytes_.size())
      continue;

    std::optional<uint64_t> delayedBoundary;
    SmallVector<MacroStateGapCarryCandidate, 4> candidates;
    for (const NamedMacroDirectiveRef &ref : plan_.namedMacroDirectives) {
      const RefoldModel::MacroDirective &directiveLocal = *ref.directive;
      if (directiveLocal.subkind != "#define")
        continue;
      if (carriedDirectiveIds.contains(directiveLocal.id))
        continue;
      if (plan_.syntheticUndefPartitionedDefinitionIds.contains(
              directiveLocal.id))
        continue;

      std::optional<MacroStateSourceTransition> transition =
          MacroStateSourceTransitionFor(directiveLocal);
      if (!transition)
        continue;
      if (transition->interval.end > edit.start)
        continue;
      if (IntervalOverlapsFinalTUEdit(transition->interval.begin,
                                      transition->interval.end))
        continue;
      if (ActiveDefinitionAtSourceOffset(ref.name, edit.start) !=
          &directiveLocal)
        continue;
      if (!ReplacementObservesPreservedDefinition(edit, directiveLocal,
                                                  ref.name))
        continue;
      if (DirectCalleePatchRequiresDefinitionBeforeReplacement(
              edit, directiveLocal, ref.name))
        continue;

      std::optional<uint64_t> boundary = DelayedTransitionBoundaryAfterEdit(
          editIndex, directiveLocal, ref.name);
      if (!boundary)
        continue;
      if (delayedBoundary && *delayedBoundary != *boundary)
        continue;
      delayedBoundary = boundary;

      // Only carry a gap definition when the B-side replacement is the first
      // material that would observe the definition.  Macro-callsite edits whose
      // emitted surface is kept in B's macro state are the important exception:
      // the original source slice is itself the macro invocation, so it
      // necessarily observes the old definition, while the accepted macro proof
      // says the replacement wants to keep the emitted callsite surface under
      // B's macro state.
      if (!EditHasMacroPatchSurfaceInBMacroState(edit) &&
          MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              directiveLocal, ref.name, tuBytes_.slice(edit.start, edit.end),
              tuBytes_.drop_front(edit.end)))
        continue;

      candidates.push_back(MacroStateGapCarryCandidate{
          &directiveLocal, transition->interval, std::move(transition->text),
          ref.name.str()});
    }

    if (candidates.empty() || !delayedBoundary)
      continue;

    llvm::sort(candidates, [](const MacroStateGapCarryCandidate &lhs,
                              const MacroStateGapCarryCandidate &rhs) {
      if (lhs.interval.begin != rhs.interval.begin)
        return lhs.interval.begin < rhs.interval.begin;
      return lhs.directive->id < rhs.directive->id;
    });

    const uint64_t newStart = candidates.front().interval.begin;
    if (SourceRangeOverlapsFinalTUEditExcept(newStart, edit.start, editIndex))
      continue;

    bool admissible = true;
    for (const MacroStateGapCarryCandidate &candidate : candidates) {
      uint64_t cursor = candidate.interval.end;
      for (const MacroStateGapCarryCandidate &other : candidates) {
        if (other.interval.begin <= cursor)
          continue;
        if (other.interval.begin > edit.start)
          break;
        StringRef chunk = tuBytes_.slice(cursor, other.interval.begin);
        StringRef following = tuBytes_.slice(other.interval.begin, edit.start);
        if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
                *candidate.directive, candidate.name, chunk, following)) {
          admissible = false;
          break;
        }
        cursor = other.interval.end;
      }
      if (!admissible)
        break;
      if (cursor < edit.start) {
        StringRef chunk = tuBytes_.slice(cursor, edit.start);
        if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
                *candidate.directive, candidate.name, chunk,
                StringRef(edit.text))) {
          admissible = false;
          break;
        }
      }
    }
    if (!admissible)
      continue;
    if (*delayedBoundary < edit.end || *delayedBoundary > tuBytes_.size())
      continue;

    std::string replacement;
    replacement.reserve((edit.start - newStart) + edit.text.size() +
                        (*delayedBoundary - edit.end) + 64);
    uint64_t cursor = newStart;
    for (const MacroStateGapCarryCandidate &candidate : candidates) {
      if (cursor > candidate.interval.begin) {
        admissible = false;
        break;
      }
      replacement.append(tuBytes_.begin() + cursor,
                         tuBytes_.begin() + candidate.interval.begin);
      cursor = candidate.interval.end;
    }
    if (!admissible)
      continue;

    replacement.append(tuBytes_.begin() + cursor,
                       tuBytes_.begin() + edit.start);
    replacement.append(edit.text);
    replacement.append(tuBytes_.begin() + edit.end,
                       tuBytes_.begin() + *delayedBoundary);
    if (!replacement.empty() && replacement.back() == '\\')
      continue;
    if (!replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');

    const uint64_t oldStart = edit.start;
    const uint64_t oldEnd = edit.end;
    SmallVector<ProvenMacroStateSourceTransition, 4> repairedTransitions;
    for (const MacroStateGapCarryCandidate &candidate : candidates) {
      const size_t finalReplacementOffset = replacement.size();
      replacement += candidate.preservationText;
      const std::string reason =
          llvm::formatv("carried observed gap #define directive #{0} for "
                        "macro '{1}' after TU replacement [{2},{3})",
                        candidate.directive->id, candidate.name, oldStart,
                        oldEnd)
              .str();
      std::optional<ProvenMacroStateSourceTransition> transition =
          ProveMacroStateSourceTransition(
              *candidate.directive, StateMutationKind::MovedLater,
              MacroStateTransitionDisposition::MovedLater,
              FinalTUStateBoundary(*delayedBoundary), finalReplacementOffset,
              SuffixStabilityWitnessKind::StateRepair,
              "macro-gap-definition-carry", reason);
      if (!transition)
        return;
      repairedTransitions.push_back(std::move(*transition));
    }

    RestageConservativeTUEdit(edit, newStart, *delayedBoundary, replacement,
                              repairedTransitions);

    for (const MacroStateGapCarryCandidate &candidate : candidates) {
      carriedDirectiveIds.insert(candidate.directive->id);
      ++carriedCount;
      REFOLD_LOG_WARN("macro/liveness",
                      "carrying observed gap #define after TU replacement: "
                      "macro='{0}' defDirective=#{1} edit=[{2},{3}) "
                      "widened=[{4},{5})",
                      candidate.name, candidate.directive->id, oldStart, oldEnd,
                      edit.start, edit.end);
    }
  }

  if (carriedCount != 0)
    REFOLD_LOG_INFO(
        "macro/liveness",
        "carried {0} preserved #define directive(s) after "
        "replacement payloads to keep edited tokens in B macro state",
        carriedCount);
}

bool MacroStateRepairContext::DefinitionDirectiveTouchedByTUEdit(
    const RefoldModel::MacroDirective &directive) const {
  if (directive.subkind != "#define")
    return false;
  return MacroDirectiveTouchedByTUEdit(directive);
}

const RefoldModel::IncludeItem *
MacroStateRepairContext::OwningIncludeSiteForInvocationInTU(
    const RefoldModel::MacroInvocation &invocation) const {
  return OutermostOwningIncludeSiteInTU(invocation.ownerIncludeId);
}

bool MacroStateRepairContext::IncludeOwnedInvocationSurvivesTUEdits(
    const RefoldModel::MacroInvocation &invocation) const {
  if (invocation.invFile &&
      PathIdentity().PathsEqual(*invocation.invFile, tuPath_))
    return false;
  const RefoldModel::IncludeItem *inc =
      OwningIncludeSiteForInvocationInTU(invocation);
  if (!inc)
    return false;
  return !IntervalOverlapsFinalTUEdit(inc->siteB, inc->siteE);
}

bool MacroStateRepairContext::InvocationCallsiteSurvivesTUEdits(
    const RefoldModel::MacroInvocation &invocation) const {
  if (!invocation.invFile || !invocation.invB || !invocation.invE ||
      *invocation.invE < *invocation.invB)
    return false;
  if (!PathIdentity().PathsEqual(*invocation.invFile, tuPath_))
    return false;
  return !IntervalOverlapsFinalTUEdit(*invocation.invB, *invocation.invE);
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::FallbackActiveDefinitionForInvocation(
    const RefoldModel::MacroInvocation &invocation) const {
  const RefoldModel::MacroDirective *best = nullptr;
  for (const NamedMacroDirectiveRef &ref : plan_.namedMacroDirectives) {
    const RefoldModel::MacroDirective &directiveLocal = *ref.directive;
    if (directiveLocal.id >= invocation.id)
      continue;
    if (StringRef(ref.name) != invocation.name)
      continue;
    if (best && best->id > directiveLocal.id)
      continue;
    best = &directiveLocal;
  }
  if (!best || best->subkind != "#define")
    return nullptr;
  return best;
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::ActiveDefinitionForInvocation(
    const RefoldModel::MacroInvocation &invocation) const {
  if (invocation.definitionDirectiveId) {
    auto it = plan_.macroDirectiveById.find(*invocation.definitionDirectiveId);
    if (it != plan_.macroDirectiveById.end() && it->second.directive &&
        it->second.directive->subkind == "#define")
      return it->second.directive;
    return nullptr;
  }
  return FallbackActiveDefinitionForInvocation(invocation);
}

bool MacroStateRepairContext::DefinitionHasOtherSurvivingSameNameTransition(
    const RefoldModel::MacroDirective &definition, StringRef macroName) const {
  for (const NamedMacroDirectiveRef &ref : plan_.namedMacroDirectives) {
    const RefoldModel::MacroDirective &directiveLocal = *ref.directive;
    if (directiveLocal.id == definition.id)
      continue;
    if (StringRef(ref.name) != macroName)
      continue;
    if (directiveLocal.subkind != "#define" &&
        directiveLocal.subkind != "#undef")
      continue;

    // A transition consumed by a final TU edit does not remain in the emitted
    // macro-state stream.  It has its own liveness proof obligation, so it
    // should not make this definition look like part of a surviving same-name
    // state chain.
    if (MacroDirectiveTouchedByTUEdit(directiveLocal))
      continue;

    return true;
  }

  return false;
}

void MacroStateRepairContext::RepairSurvivingDefinitionCallsites() {
  size_t preservedDefinitionLivenessDirectives = 0;
  size_t forcedDefinitionLivenessPatches = 0;

  for (const RefoldModel::MacroInvocation &invocation :
       Model().GetMacroInvocations()) {
    if (invocation.callerMacroId)
      continue;
    if (MacroTopology().IsInvocationInsideDefineDirective(invocation))
      continue;

    const bool survivesAsTUCallsite =
        InvocationCallsiteSurvivesTUEdits(invocation);
    const bool survivesAsIncludeCallsite =
        IncludeOwnedInvocationSurvivesTUEdits(invocation);
    if (!survivesAsTUCallsite && !survivesAsIncludeCallsite)
      continue;

    const RefoldModel::MacroDirective *definition =
        ActiveDefinitionForInvocation(invocation);
    if (!definition || !DefinitionDirectiveTouchedByTUEdit(*definition))
      continue;

    if (PhysicalCallsiteIsMaterialized(invocation))
      continue;

    bool preservedDefinition = false;
    const bool hasOtherSurvivingSameNameTransition =
        DefinitionHasOtherSurvivingSameNameTransition(*definition,
                                                      invocation.name);
    const bool mayPreserveDefinition =
        survivesAsIncludeCallsite ||
        (survivesAsTUCallsite && !hasOtherSurvivingSameNameTransition);
    if (mayPreserveDefinition) {
      if (std::optional<size_t> editIndex =
              FinalTUEditContainingMacroDirective(*definition)) {
        std::optional<MacroStatePreservationPlacement> placement;
        if (!plan_.preservedDefinitionDirectiveIds.contains(definition->id))
          placement = TryQueueMacroStateDirectivePreservation(
              *editIndex, *definition, invocation.name,
              /*ObservedDefinition=*/nullptr);
        else
          placement = MacroStatePreservationPlacement::BeforeReplacement;

        if (placement) {
          preservedDefinition = true;
          if (!plan_.preservedDefinitionDirectiveIds.contains(definition->id)) {
            plan_.preservedDefinitionDirectiveIds.insert(definition->id);
            ++preservedDefinitionLivenessDirectives;
            REFOLD_LOG_WARN(
                "macro/liveness",
                "preserving consumed #define for surviving macro callsite: "
                "macro='{0}' defDirective=#{1} inv=#{2} edit=[{3},{4}) "
                "placement={5}",
                invocation.name, definition->id, invocation.id,
                tuEdits_[*editIndex].start, tuEdits_[*editIndex].end,
                MacroStatePreservationPlacementName(*placement));
          }
        }
      }
    }

    if (preservedDefinition) {
      // The queued preservation is proved and authorized atomically after all
      // replacement-local insertion offsets are finalized.  Performing a
      // detached gateway check here would create proof evidence unrelated to
      // the exact transition capability eventually minted for the edit.
      continue;
    }

    if (survivesAsIncludeCallsite) {
      (void)CheckMacroStateTerminal(
          *definition, StateMutationKind::Consumed, "macro-definition-liveness",
          llvm::formatv("macro '{0}' invocation #{1} survives inside preserved "
                        "include site, but active definition directive #{2} "
                        "was consumed by a TU edit and the directive could not "
                        "be preserved in any proved placement before the "
                        "surviving include observes macro state",
                        invocation.name, invocation.id, definition->id)
              .str(),
          /*RequireKnownObserver=*/true, invocation.id);
      continue;
    }

    // If the #define cannot be safely preserved, the only structural repair is
    // to remove this call site's dependency on that macro state by emitting the
    // B-side expansion at the call site.
    std::optional<WholeCoverPlan> wholePlan =
        MacroPatchPlanner().ComputeWholeCoverPlan(invocation);
    if (!wholePlan) {
      (void)CheckMacroStateTerminal(
          *definition, StateMutationKind::Consumed, "macro-definition-liveness",
          llvm::formatv("macro '{0}' invocation #{1} survives but active "
                        "definition directive #{2} was consumed by a TU edit; "
                        "the #define could not be preserved in any proved "
                        "placement and no whole-cover realization is available",
                        invocation.name, invocation.id, definition->id)
              .str(),
          /*RequireKnownObserver=*/true, invocation.id);
      continue;
    }

    MacroPatch patch{*invocation.invB, *invocation.invE, wholePlan->clippedText,
                     invocation.id};
    ProofLattice().CertifyMacroWholeCoverRealizationPatch(patch, *wholePlan,
                                                          invocation);
    MacroPatchPlanner().CertifyMacroPatchOwnerWitness(
        patch, invocation.ownerIncludeId
                   ? Owner::Include(*invocation.ownerIncludeId)
                   : Owner::TU());

    // Reuse an existing physical-callsite key if one was already allocated for
    // the same byte interval. Otherwise use this invocation id as the stable
    // key for the forced whole-cover patch.
    RefoldStructuralHunkDispatcher::MacroPatchStagingSlot stagingSlot =
        dispatcher_.PrepareMacroPatchStagingSlot(invocation);
    dispatcher_.StageMacroPatch(stagingSlot, std::move(patch));
    ++forcedDefinitionLivenessPatches;

    REFOLD_LOG_WARN("macro/liveness",
                    "forced whole-cover macro realization because active "
                    "definition was consumed by TU edit and could not be "
                    "preserved: macro='{0}' defDirective=#{1} inv=#{2} "
                    "invBytes=[{3},{4})",
                    invocation.name, definition->id, invocation.id,
                    *invocation.invB, *invocation.invE);
  }

  if (TerminalSink().HasRequest())
    return;

  if (preservedDefinitionLivenessDirectives != 0) {
    REFOLD_LOG_INFO("macro/liveness",
                    "preserved {0} consumed #define directive(s) needed by "
                    "surviving macro callsite(s)",
                    preservedDefinitionLivenessDirectives);
  }

  if (forcedDefinitionLivenessPatches != 0) {
    REFOLD_LOG_INFO("macro/liveness",
                    "forced {0} macro callsite(s) to whole-cover realization "
                    "after definition-removing TU edits",
                    forcedDefinitionLivenessPatches);
  }
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::PreviousLiveDefinitionBeforeDirective(
    const NamedMacroDirectiveRef &undefRef) const {
  const RefoldModel::MacroDirective *active = nullptr;
  for (const NamedMacroDirectiveRef &ref : plan_.namedMacroDirectives) {
    const RefoldModel::MacroDirective &directiveLocal = *ref.directive;
    if (directiveLocal.id >= undefRef.directive->id)
      break;
    if (StringRef(ref.name) != StringRef(undefRef.name))
      continue;
    if (directiveLocal.subkind == "#define")
      active = &directiveLocal;
    else if (directiveLocal.subkind == "#undef")
      active = nullptr;
  }
  return active;
}

bool MacroStateRepairContext::DirectiveSurvivesTUEdits(
    const RefoldModel::MacroDirective &directive) const {
  return !MacroDirectiveTouchedByTUEdit(directive);
}

size_t MacroStateRepairContext::PreserveConsumedUndefs() {
  size_t undefLivenessHazards = 0;
  for (const NamedMacroDirectiveRef &ref : plan_.namedMacroDirectives) {
    const RefoldModel::MacroDirective &undefDirective = *ref.directive;
    if (undefDirective.subkind != "#undef")
      continue;

    std::optional<size_t> editIndex =
        FinalTUEditContainingMacroDirective(undefDirective);
    if (!editIndex)
      continue;

    const RefoldModel::MacroDirective *previousDefinition =
        PreviousLiveDefinitionBeforeDirective(ref);
    if (!previousDefinition || !DirectiveSurvivesTUEdits(*previousDefinition))
      continue;

    std::optional<MacroStatePreservationPlacement> placement =
        TryQueueMacroStateDirectivePreservation(*editIndex, undefDirective,
                                                ref.name, previousDefinition);
    if (!placement) {
      (void)CheckMacroStateTerminal(
          undefDirective, StateMutationKind::Consumed, "macro-undef-liveness",
          llvm::formatv("#undef directive #{0} for macro '{1}' was consumed "
                        "by TU edit [{2},{3}), but it could not be preserved "
                        "without exposing the replacement payload to the macro "
                        "name or breaking the replacement/suffix boundary",
                        undefDirective.id, ref.name, tuEdits_[*editIndex].start,
                        tuEdits_[*editIndex].end)
              .str(),
          /*RequireKnownObserver=*/true);
      continue;
    }

    ++undefLivenessHazards;
    REFOLD_LOG_WARN(
        "macro/liveness",
        "preserving consumed #undef to prevent resurrected macro "
        "definition: macro='{0}' undefDirective=#{1} priorDefine=#{2} "
        "edit=[{3},{4}) placement={5}",
        ref.name, undefDirective.id, previousDefinition->id,
        tuEdits_[*editIndex].start, tuEdits_[*editIndex].end,
        MacroStatePreservationPlacementName(*placement));
  }

  return undefLivenessHazards;
}

bool MacroStateRepairContext::ApplyQueuedMacroStatePreservations() {
  struct PreservationEmission {
    const MacroStatePreservation *preservation = nullptr;
    size_t replacementOffset = 0;
  };

  for (auto &entry : macroStatePreservationsByEdit_) {
    if (entry.first >= tuEdits_.size()) {
      TerminalSink().RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "macro/state-repair",
          "queued macro-state preservation references a missing TU edit");
      return false;
    }

    TextEdit &edit = tuEdits_[entry.first];
    SmallVector<MacroStatePreservation, 8> preservations(entry.second.begin(),
                                                         entry.second.end());
    llvm::sort(preservations, [](const MacroStatePreservation &lhs,
                                 const MacroStatePreservation &rhs) {
      if (lhs.placement != rhs.placement)
        return lhs.placement < rhs.placement;
      if (lhs.replacementOffset != rhs.replacementOffset)
        return lhs.replacementOffset < rhs.replacementOffset;
      if (lhs.directive->siteB != rhs.directive->siteB)
        return lhs.directive->siteB < rhs.directive->siteB;
      return lhs.directive->id < rhs.directive->id;
    });

    // Emit the repaired replacement once while recording the exact final
    // replacement-local boundary of every replayed directive.  This avoids a
    // second text search and keeps the state proof tied to the bytes that will
    // actually be emitted.
    const std::string originalReplacement = edit.text;
    std::string repairedReplacement;
    SmallVector<PreservationEmission, 8> emissions;
    DenseSet<uint64_t> emittedDirectiveIds;

    auto appendPreservation = [&](const MacroStatePreservation &preservation) {
      if (!preservation.directive ||
          !emittedDirectiveIds.insert(preservation.directive->id).second)
        return;
      emissions.push_back(
          PreservationEmission{&preservation, repairedReplacement.size()});
      repairedReplacement +=
          DirectiveTextForPreservation(*preservation.directive);
    };

    for (const MacroStatePreservation &preservation : preservations) {
      if (preservation.placement ==
          MacroStatePreservationPlacement::BeforeReplacement)
        appendPreservation(preservation);
    }

    size_t cursor = 0;
    for (const MacroStatePreservation &preservation : preservations) {
      if (preservation.placement !=
          MacroStatePreservationPlacement::InsideReplacement)
        continue;
      const size_t offset =
          std::min(preservation.replacementOffset, originalReplacement.size());
      if (offset < cursor) {
        TerminalSink().RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::EmissionEditSetComposable,
                TerminalFallbackFailureReason::UncomposableEmissionEditSet),
            "macro/state-repair",
            "queued macro-state preservation offsets are non-monotone");
        return false;
      }
      repairedReplacement.append(originalReplacement.begin() + cursor,
                                 originalReplacement.begin() + offset);
      cursor = offset;
      appendPreservation(preservation);
    }
    repairedReplacement.append(originalReplacement.begin() + cursor,
                               originalReplacement.end());

    bool appendedAfterReplacementSeparator = false;
    for (const MacroStatePreservation &preservation : preservations) {
      if (preservation.placement !=
          MacroStatePreservationPlacement::AfterReplacement)
        continue;
      if (!appendedAfterReplacementSeparator) {
        if (!repairedReplacement.empty() && repairedReplacement.back() != '\n')
          repairedReplacement.push_back('\n');
        appendedAfterReplacementSeparator = true;
      }
      appendPreservation(preservation);
    }

    SmallVector<ProvenMacroStateSourceTransition, 8> provedTransitions;
    provedTransitions.reserve(emissions.size());
    for (const PreservationEmission &emission : emissions) {
      const MacroStatePreservation &preservation = *emission.preservation;
      const RefoldModel::MacroDirective &directive = *preservation.directive;

      MacroStateTransitionDisposition disposition =
          MacroStateTransitionDisposition::Unknown;
      uint64_t finalSourceOffset = edit.start;
      switch (preservation.placement) {
      case MacroStatePreservationPlacement::BeforeReplacement:
        disposition =
            MacroStateTransitionDisposition::ReplayedBeforeReplacement;
        break;
      case MacroStatePreservationPlacement::InsideReplacement:
        disposition =
            MacroStateTransitionDisposition::ReplayedInsideReplacement;
        break;
      case MacroStatePreservationPlacement::AfterReplacement:
        disposition =
            MacroStateTransitionDisposition::ReplayedAfterReplacement;
        finalSourceOffset = edit.end;
        break;
      case MacroStatePreservationPlacement::AdvancedBeforeReplacement:
        // Advanced repairs were proved, restaged, and authorized at the source
        // boundary where the edit was widened. They intentionally contribute
        // no second replay surface here.
        continue;
      }

      const std::string reason =
          llvm::formatv("preserved consumed {0} directive #{1} for macro "
                        "'{2}' using {3} placement",
                        directive.subkind, directive.id, directive.name,
                        MacroStatePreservationPlacementName(
                            preservation.placement))
              .str();
      std::optional<ProvenMacroStateSourceTransition> transition =
          ProveMacroStateSourceTransition(
              directive,
              MutationForMacroStatePreservationPlacement(
                  preservation.placement),
              disposition, FinalTUStateBoundary(finalSourceOffset),
              emission.replacementOffset,
              SuffixStabilityWitnessKind::StateRepair,
              "macro-state-preservation", reason);
      if (!transition)
        return false;
      provedTransitions.push_back(std::move(*transition));
    }

    edit.text = std::move(repairedReplacement);
    for (const ProvenMacroStateSourceTransition &transition :
         provedTransitions) {
      if (!AuthorizeMacroStateSourceTransition(edit, transition))
        return false;
    }
    PromoteToSpecializedMacroStateRepairCarrier(edit);
  }
  return true;
}

bool MacroStateRepairContext::RunInitialRepair() {
  // Preserve or move macro-state transitions whose source order was invalidated
  // by already-staged TU edits.  These phases deliberately run before forced
  // macro-callsite realization so the source-preserving repair wins when it is
  // provable.
  AdvancePreservedUndefsBeforeObservedReplacements();
  SynthesizeUndefBeforeObservedGapDefinitions();
  CarryObservedGapDefinitionsAfterReplacements();

  RepairSurvivingDefinitionCallsites();
  if (TerminalSink().HasRequest()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted after macro-definition "
                     "liveness repair; terminal fallback will be emitted");
    return false;
  }

  // Direct span planning contributes no macro-state authority. Authorize an
  // unreplayed consumed definition only after the specialized planner proves
  // that every producer-linked physical callsite has been materialized by an
  // accepted TU edit or macro patch.
  if (!AuthorizeMaterializedDefinitionTransitions()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted while authorizing "
                     "materialized macro definitions; terminal fallback will "
                     "be emitted");
    return false;
  }

  const size_t undefLivenessHazards = PreserveConsumedUndefs();
  if (TerminalSink().HasRequest()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted after macro-undef liveness "
                     "repair; terminal fallback will be emitted");
    return false;
  }

  if (!ApplyQueuedMacroStatePreservations() || TerminalSink().HasRequest()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted while authorizing exact "
                     "macro-state repair transitions; terminal fallback will "
                     "be emitted");
    return false;
  }

  if (undefLivenessHazards != 0) {
    REFOLD_LOG_INFO("macro/liveness",
                    "preserved {0} consumed #undef directive(s) to keep suffix "
                    "macro state equivalent after source replacement",
                    undefLivenessHazards);
  }

  return true;
}

bool MacroStateRepairContext::MaterializedIncludeSubtreeOwnsMacroDirective(
    const RefoldModel::IncludeItem &root,
    const RefoldModel::MacroDirective &directive) const {
  if (!directive.ownerIncludeId)
    return false;
  const RefoldModel::IncludeItem *cur =
      Model().GetIncludeById(*directive.ownerIncludeId);
  while (cur) {
    if (cur->id == root.id)
      return true;
    if (!cur->parent)
      return false;
    cur = Model().GetIncludeById(*cur->parent);
  }
  return false;
}

bool MacroStateRepairContext::MaterializedReplacementPreservesDirectiveAncestry(
    const RefoldModel::IncludeItem &root,
    const RefoldModel::MacroDirective &directive,
    const TextEdit &replacementEdit) const {
  if (!directive.ownerIncludeId)
    return false;
  const RefoldModel::IncludeItem *cur =
      Model().GetIncludeById(*directive.ownerIncludeId);
  while (cur) {
    if (IncludeDirectiveAppearsAtLineStart(replacementEdit, *cur))
      return true;
    if (cur->id == root.id)
      break;
    if (!cur->parent)
      break;
    cur = Model().GetIncludeById(*cur->parent);
  }
  return false;
}

bool MacroStateRepairContext::InvocationSurvivesAfterMaterializedInclude(
    const RefoldModel::MacroInvocation &invocation,
    const RefoldModel::IncludeItem &materializedInclude,
    uint64_t materializedSiteEnd) const {
  if (InvocationCallsiteSurvivesTUEdits(invocation))
    return invocation.invB && *invocation.invB >= materializedSiteEnd;

  if (!IncludeOwnedInvocationSurvivesTUEdits(invocation))
    return false;
  const RefoldModel::IncludeItem *owner =
      OwningIncludeSiteForInvocationInTU(invocation);
  return owner && owner->siteB >= materializedSiteEnd &&
         owner->id != materializedInclude.id;
}

bool MacroStateRepairContext::MaterializedIncludeNeedsDefinitionAfterward(
    const RefoldModel::IncludeItem &materializedInclude,
    uint64_t materializedSiteEnd,
    const RefoldModel::MacroDirective &definition) const {
  for (const RefoldModel::MacroInvocation &invocation :
       Model().GetMacroInvocations()) {
    if (invocation.callerMacroId ||
        MacroTopology().IsInvocationInsideDefineDirective(invocation))
      continue;
    if (PhysicalCallsiteIsMaterialized(invocation))
      continue;
    const RefoldModel::MacroDirective *active =
        ActiveDefinitionForInvocation(invocation);
    if (!active || active->id != definition.id)
      continue;
    if (InvocationSurvivesAfterMaterializedInclude(
            invocation, materializedInclude, materializedSiteEnd))
      return true;
  }
  return false;
}

/// A carried definition is only usable if every macro its replacement list
/// names is still defined where the carried copy lands.  The producer records
/// the replacement list as a token tape, so the reference is read from that
/// tape rather than rediscovered by reparsing directive text.  Parameter
/// references cannot name another macro and are skipped.
bool MacroStateRepairContext::DefinitionReplacementListNamesMacro(
    const RefoldModel::MacroDirective &definition, StringRef macroName) const {
  if (macroName.empty())
    return false;
  for (const RefoldModel::MacroReplacementToken &token :
       definition.replacementTokens) {
    if (token.kind != RefoldModel::MacroReplacementTokenKind::Literal)
      continue;
    if (token.spelling == macroName)
      return true;
  }
  return false;
}

bool MacroStateRepairContext::RepairConsumedDefinitionsForMaterializedInclude(
    const RefoldModel::IncludeItem &materializedInclude,
    uint64_t materializedSiteBegin, uint64_t materializedSiteEnd,
    std::string &replacementText) {
  TextEdit replacementProbe{materializedSiteBegin,
                            materializedSiteEnd,
                            replacementText,
                            std::nullopt,
                            std::nullopt,
                            {},
                            {},
                            {}};
  std::string preservedDirectivePrefix;

  // Every definition this materialized include would consume and that the
  // replacement does not already carry.
  SmallVector<const NamedMacroDirectiveRef *, 8> candidates;
  for (const NamedMacroDirectiveRef &ref : plan_.namedMacroDirectives) {
    const RefoldModel::MacroDirective &definition = *ref.directive;
    if (definition.subkind != "#define")
      continue;
    if (plan_.preservedDefinitionDirectiveIds.contains(definition.id))
      continue;
    if (!MaterializedIncludeSubtreeOwnsMacroDirective(materializedInclude,
                                                      definition))
      continue;
    if (MaterializedReplacementPreservesDirectiveAncestry(
            materializedInclude, definition, replacementProbe))
      continue;
    if (MacroStateDirectiveAppearsAtLineStart(replacementProbe, definition))
      continue;
    candidates.push_back(&ref);
  }

  // Seed the carry set with the definitions a surviving suffix observer names
  // directly.
  DenseSet<uint64_t> carriedDefinitionIds;
  for (const NamedMacroDirectiveRef *ref : candidates) {
    if (MaterializedIncludeNeedsDefinitionAfterward(
            materializedInclude, materializedSiteEnd, *ref->directive))
      carriedDefinitionIds.insert(ref->directive->id);
  }

  // Close that set over replacement-list references.
  //
  // A surviving observer names the macro it invokes, not the macros that
  // invocation expands into, so the seed is only the outermost layer.  Carrying
  // `usbi_err` while consuming the `_usbi_log` its body names would emit a
  // definition that no longer expands: the identifier survives into the output
  // and the refolded source stops matching the edited stream.  A definition
  // therefore travels with every consumed definition its replacement list
  // names, transitively.
  //
  // Candidates are visited in producer record order, which is the order the
  // directives appear in the header, so a definition is emitted before the one
  // whose body names it -- the order the original source already proved works.
  for (bool changed = true; changed;) {
    changed = false;
    for (const NamedMacroDirectiveRef *ref : candidates) {
      if (carriedDefinitionIds.contains(ref->directive->id))
        continue;
      for (const NamedMacroDirectiveRef *carried : candidates) {
        if (!carriedDefinitionIds.contains(carried->directive->id))
          continue;
        if (!DefinitionReplacementListNamesMacro(*carried->directive,
                                                 ref->name))
          continue;
        REFOLD_LOG_TRACE(
            "include/materialized-macro-state",
            "inc#{0}: carrying definition #{1} for macro '{2}' because "
            "carried definition #{3} names it in its replacement list",
            materializedInclude.id, ref->directive->id, ref->name,
            carried->directive->id);
        carriedDefinitionIds.insert(ref->directive->id);
        changed = true;
        break;
      }
    }
  }

  for (const NamedMacroDirectiveRef *refPtr : candidates) {
    const NamedMacroDirectiveRef &ref = *refPtr;
    const RefoldModel::MacroDirective &definition = *ref.directive;
    if (!carriedDefinitionIds.contains(definition.id))
      continue;

    if (DefinitionHasOtherSurvivingSameNameTransition(definition, ref.name)) {
      (void)CheckMacroStateTerminal(
          definition, StateMutationKind::Consumed,
          "include/materialized-macro-state",
          llvm::formatv("materialized include inc#{0} consumes definition #{1} "
                        "for macro '{2}', but another same-name transition "
                        "survives in the suffix",
                        materializedInclude.id, definition.id, ref.name)
              .str(),
          /*RequireKnownObserver=*/true, materializedInclude.id);
      return false;
    }

    if (FirstReplacementObservationOffset(replacementProbe, definition,
                                          ref.name)) {
      (void)CheckMacroStateTerminal(
          definition, StateMutationKind::Consumed,
          "include/materialized-macro-state",
          llvm::formatv("materialized include inc#{0} consumes definition #{1} "
                        "for macro '{2}', but the materialized payload itself "
                        "observes that macro",
                        materializedInclude.id, definition.id, ref.name)
              .str(),
          /*RequireKnownObserver=*/true, materializedInclude.id);
      return false;
    }

    std::string directiveText = DirectiveTextForPreservation(definition);
    if (directiveText.empty())
      continue;
    if (!directiveText.empty() && directiveText.back() != '\n')
      directiveText.push_back('\n');

    (void)CheckMacroStateRepaired(
        definition, StateMutationKind::Replayed,
        "include/materialized-macro-state",
        llvm::formatv("preserved include-owned definition #{0} before "
                      "materialized include inc#{1} for surviving suffix macro "
                      "'{2}'",
                      definition.id, materializedInclude.id, ref.name)
            .str());

    preservedDirectivePrefix += directiveText;
    plan_.preservedDefinitionDirectiveIds.insert(definition.id);
  }

  if (!preservedDirectivePrefix.empty())
    replacementText.insert(0, preservedDirectivePrefix);

  return true;
}

} // namespace

RefoldMacroStateRepairPlanner::RefoldMacroStateRepairPlanner(Dependencies deps)
    : deps_(deps) {}

RefoldMacroStateRepairPlanner::MacroStateRepairPlan
RefoldMacroStateRepairPlanner::Plan(
    const MacroStateRepairRequest &request) const {
  MacroStateRepairPlan repairPlan;
  MacroStateRepairContext Context(deps_, request, repairPlan);
  repairPlan.success = Context.RunInitialRepair();
  return repairPlan;
}

void RefoldMacroStateRepairPlanner::
    CarryObservedGapDefinitionsAfterReplacements(
        MacroStateRepairPlan &plan,
        const MacroStateRepairRequest &request) const {
  MacroStateRepairContext Context(deps_, request, plan);
  Context.CarryObservedGapDefinitionsAfterReplacements();
}

bool RefoldMacroStateRepairPlanner::
    RepairConsumedDefinitionsForMaterializedInclude(
        MacroStateRepairPlan &plan, const MacroStateRepairRequest &request,
        const RefoldModel::IncludeItem &materializedInclude,
        uint64_t materializedSiteBegin, uint64_t materializedSiteEnd,
        std::string &replacementText) const {
  MacroStateRepairContext Context(deps_, request, plan);
  return Context.RepairConsumedDefinitionsForMaterializedInclude(
      materializedInclude, materializedSiteBegin, materializedSiteEnd,
      replacementText);
}

} // namespace refold
} // namespace clang

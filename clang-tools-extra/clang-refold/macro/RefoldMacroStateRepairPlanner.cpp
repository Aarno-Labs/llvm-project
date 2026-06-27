//===--- RefoldMacroStateRepairPlanner.cpp ---------------------*- C++ -*-===//
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
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldTerminalProofSink.h"
#include "proof/RefoldProofTypes.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "source/TokenTextHelpers.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
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
  const RefoldModel::MacroDirective *Directive = nullptr;
  MacroStatePreservationPlacement Placement =
      MacroStatePreservationPlacement::BeforeReplacement;
  size_t ReplacementOffset = 0;
};

struct MacroStateSourceTransition {
  MacroDirectiveSourceInterval Interval;
  std::string Text;
};

struct MacroStateGapCarryCandidate {
  const RefoldModel::MacroDirective *Directive = nullptr;
  MacroDirectiveSourceInterval Interval;
  std::string PreservationText;
  std::string Name;
};

struct SyntheticUndefCandidate {
  const RefoldModel::MacroDirective *Definition = nullptr;
  MacroDirectiveSourceInterval Interval;
  std::string Name;
};

class MacroStateRepairContext {
public:
  /// Creates a repair context for one planner phase and builds the shared macro
  /// directive index exactly once for that phase invocation.
  MacroStateRepairContext(const Dependencies &Deps,
                          const MacroStateRepairRequest &Request,
                          MacroStateRepairPlan &Plan)
      : deps_(Deps), plan_(Plan),
        dispatcher_(*Request.StructuralHunkDispatcher),
        tuEdits_(*Request.TUEdits), tuPath_(Request.TUPath),
        tuBytes_(Request.TUBytes) {
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
      const RefoldModel::IncludeItem &MaterializedInclude,
      uint64_t MaterializedSiteBegin, uint64_t MaterializedSiteEnd,
      std::string &ReplacementText);

private:
  /// Returns the immutable refold model used for macro/include queries.
  const RefoldModel &Model() const { return *deps_.Model; }
  /// Returns the path identity service used for physical source comparisons.
  const RefoldPathIdentity &PathIdentity() const { return *deps_.PathIdentity; }
  /// Returns the macro topology service used to inspect expansion relations.
  const RefoldMacroTopology &MacroTopology() const {
    return *deps_.MacroTopology;
  }
  /// Returns token-text analysis helpers for replacement observation checks.
  const RefoldTokenTextAnalysis &TokenTextAnalysis() const {
    return *deps_.TokenTextAnalysis;
  }
  /// Returns macro-state proof helpers used to locate observations and witnesses.
  const RefoldMacroStateProof &MacroStateProof() const {
    return *deps_.MacroStateProof;
  }
  /// Returns owner-state proof machinery used to validate repair transitions.
  RefoldOwnerStateProof &OwnerStateProof() const {
    return *deps_.OwnerStateProof;
  }
  /// Returns the proof lattice used when restaging conservative TU edits.
  RefoldProofLattice &ProofLattice() const { return *deps_.ProofLattice; }
  /// Returns the macro patch planner used to detect existing macro surfaces.
  RefoldMacroPatchPlanner &MacroPatchPlanner() const {
    return *deps_.MacroPatchPlanner;
  }
  /// Returns the edit assembler used to restage and stamp repair edits.
  RefoldTextEditAssembler &TextEditAssembler() const {
    return *deps_.TextEditAssembler;
  }
  /// Returns the terminal proof sink used for conservative fallback evidence.
  RefoldTerminalProofSink &TerminalSink() const { return *deps_.TerminalSink; }
  /// Returns lexer language options used by boundary and token scanners.
  const clang::LangOptions &LexLang() const { return *deps_.LexLang; }

  /// Populates the plan's macro directive lookup tables if they have not
  /// already been built by an earlier repair phase.
  void BuildDirectiveIndexOnce();

  /// Returns whether two half-open source intervals overlap.
  static bool SourceIntervalsOverlap(uint64_t ABegin, uint64_t AEnd,
                                     uint64_t BBegin, uint64_t BEnd);
  /// Returns whether a source interval overlaps any final TU edit.
  bool IntervalOverlapsFinalTUEdit(uint64_t Begin, uint64_t End) const;
  /// Finds the final TU edit that fully contains a source interval, if any.
  std::optional<size_t> FinalTUEditContainingInterval(uint64_t Begin,
                                                      uint64_t End) const;
  /// Returns whether a source range overlaps a final TU edit other than the
  /// explicitly excluded edit index.
  bool SourceRangeOverlapsFinalTUEditExcept(uint64_t Begin, uint64_t End,
                                            size_t ExceptEditIndex) const;

  /// Finds the outermost TU-visible include site for an include ownership chain.
  const RefoldModel::IncludeItem *
  OutermostOwningIncludeSiteInTU(std::optional<uint64_t> IncludeId) const;
  /// Finds the TU-visible include site that owns a macro directive, if any.
  const RefoldModel::IncludeItem *
  OwningIncludeSiteInTU(const RefoldModel::MacroDirective &Directive) const;
  /// Returns the original include directive text for an include item.
  StringRef IncludeDirectiveText(const RefoldModel::IncludeItem &Inc) const;
  /// Returns whether an include directive is preserved at a line-start boundary
  /// inside a replacement edit.
  bool IncludeDirectiveAppearsAtLineStart(
      const TextEdit &Edit, const RefoldModel::IncludeItem &Inc) const;
  /// Returns whether a TU-visible include site survives or is exactly preserved
  /// by the final TU edit stream.
  bool IncludeSitePreservedByFinalTUEdit(
      const RefoldModel::IncludeItem &Inc) const;
  /// Returns whether the include ancestry needed for an include-owned directive
  /// remains represented after final TU edits.
  bool IncludeAncestrySitePreservedByFinalTUEdit(
      const RefoldModel::MacroDirective &Directive) const;

  /// Computes the full physical source interval occupied by a macro directive.
  std::optional<MacroDirectiveSourceInterval>
  MacroDirectiveFullSourceInterval(
      const RefoldModel::MacroDirective &Directive) const;
  /// Returns whether a macro-state directive appears at a line-start boundary
  /// in a replacement edit.
  bool MacroStateDirectiveAppearsAtLineStart(
      const TextEdit &Edit, const RefoldModel::MacroDirective &Directive) const;
  /// Finds the final TU edit that fully contains a macro directive, if any.
  std::optional<size_t> FinalTUEditContainingMacroDirective(
      const RefoldModel::MacroDirective &Directive) const;
  /// Returns whether a macro directive is touched by any final TU edit.
  bool MacroDirectiveTouchedByTUEdit(
      const RefoldModel::MacroDirective &Directive) const;
  /// Builds the directive spelling used when preserving a consumed macro-state
  /// transition in a repair edit.
  std::string DirectiveTextForPreservation(
      const RefoldModel::MacroDirective &Directive) const;

  /// Reconstructs the macro-state transition represented by a source directive.
  std::optional<MacroStateSourceTransition> MacroStateSourceTransitionFor(
      const RefoldModel::MacroDirective &Directive) const;
  /// Finds the active definition for a macro name at a physical source offset.
  const RefoldModel::MacroDirective *
  ActiveDefinitionAtSourceOffset(StringRef MacroName, uint64_t Offset) const;
  /// Returns whether a consumed macro-state directive can be delayed after a
  /// replacement edit without changing observer semantics.
  bool MacroStateDirectiveCanBeDelayedAfterEdit(
      const TextEdit &Edit, const RefoldModel::MacroDirective &Directive) const;
  /// Returns whether a definition is already available before a source offset.
  bool MacroStateDefinitionAvailableBeforeSourceOffset(
      const RefoldModel::MacroDirective &Definition, uint64_t Offset) const;

  /// Finds the first replacement-text observation of a preserved definition.
  std::optional<size_t> FirstReplacementObservationOffset(
      const TextEdit &Edit, const RefoldModel::MacroDirective &Definition,
      StringRef MacroName) const;
  /// Returns whether replacement text observes the preserved definition state.
  bool ReplacementObservesPreservedDefinition(
      const TextEdit &Edit, const RefoldModel::MacroDirective &Definition,
      StringRef MacroName) const;
  /// Finds the replacement line start that precedes a macro observation.
  std::optional<size_t> ReplacementLineStartBeforeObservation(
      StringRef Replacement, size_t ObservationOffset) const;
  /// Finds the line start after the final replacement observation of a macro.
  std::optional<size_t> ReplacementLineStartAfterFinalObservation(
      const TextEdit &Edit, const RefoldModel::MacroDirective &Definition,
      StringRef MacroName, size_t FirstObservationOffset) const;
  /// Returns whether a replacement suffix boundary can safely host a directive
  /// line without merging into source text.
  bool ReplacementSuffixBoundaryAllowsDirectiveLine(const TextEdit &Edit) const;
  /// Returns whether an edit already carries a macro patch surface in B-state.
  bool EditHasMacroPatchSurfaceInBMacroState(const TextEdit &Edit) const;

  /// Builds the owner-state boundary immediately after a macro directive.
  OwnerStateBoundary MacroDirectiveSuffixBoundary(
      const RefoldModel::MacroDirective &Directive) const;
  /// Validates a macro-state transition across an edit boundary using an
  /// explicit suffix-stability witness.
  StateTransitionProof CheckMacroStateWithWitness(
      const OwnerStateBoundary &Boundary, StateMutationKind Mutation,
      SuffixStabilityWitness Witness, StringRef Stage, StringRef Detail,
      bool RequireKnownObserver) const;
  /// Validates proof evidence for a repaired macro-state transition.
  StateTransitionProof CheckMacroStateRepaired(
      const RefoldModel::MacroDirective &Directive, StateMutationKind Mutation,
      StringRef Stage, StringRef Detail,
      bool RequireKnownObserver = false) const;
  /// Validates proof evidence for a materialized macro-state transition.
  StateTransitionProof CheckMacroStateMaterialized(
      const RefoldModel::MacroDirective &Directive, StateMutationKind Mutation,
      StringRef Stage, StringRef Detail,
      bool RequireKnownObserver = false) const;
  /// Validates terminal fallback evidence for a macro-state transition.
  StateTransitionProof CheckMacroStateTerminal(
      const RefoldModel::MacroDirective &Directive, StateMutationKind Mutation,
      StringRef Stage, StringRef Detail,
      bool RequireKnownObserver = true) const;
  /// Maps a preservation placement to the corresponding owner-state mutation.
  StateMutationKind MutationForMacroStatePreservationPlacement(
      MacroStatePreservationPlacement Placement) const;
  /// Returns a stable diagnostic name for a preservation placement.
  StringRef MacroStatePreservationPlacementName(
      MacroStatePreservationPlacement Placement) const;

  /// Computes a delayed macro-state boundary after an edit when the source
  /// suffix can safely carry the directive line.
  std::optional<uint64_t> DelayedTransitionBoundaryAfterEdit(
      size_t EditIndex, const RefoldModel::MacroDirective &Definition,
      StringRef MacroName) const;
  /// Attempts to move a consumed undef before an observed replacement that
  /// depends on the previous live definition.
  std::optional<MacroStatePreservationPlacement>
  TryAdvanceConsumedUndefBeforeObservedReplacement(
      size_t EditIndex, const RefoldModel::MacroDirective &UndefDirective,
      StringRef MacroName,
      const RefoldModel::MacroDirective &PreviousDefinition,
      size_t FirstObservationOffset);
  /// Attempts to widen an edit to a delayed boundary that preserves macro-state
  /// liveness without creating an extra directive line.
  bool TryWidenEditToDelayedMacroStateBoundary(
      size_t EditIndex, const RefoldModel::MacroDirective &Definition,
      StringRef MacroName);
  /// Queues a macro-state directive preservation for an edit when proof and
  /// placement constraints allow it.
  std::optional<MacroStatePreservationPlacement>
  TryQueueMacroStateDirectivePreservation(
      size_t EditIndex, const RefoldModel::MacroDirective &Directive,
      StringRef MacroName,
      const RefoldModel::MacroDirective *ObservedDefinition);

  /// Restages a conservative TU edit after a repair changes its byte range or
  /// replacement text.
  void RestageConservativeTUEdit(TextEdit &Edit, uint64_t Start, uint64_t End,
                                 StringRef Replacement);
  /// Attaches conservative TU proof carrier metadata to a repaired edit.
  void AttachConservativeTUCarrier(TextEdit &Edit);

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
  /// Applies queued macro-state preservation edits in deterministic edit order.
  void ApplyQueuedMacroStatePreservations();

  /// Returns whether a definition directive is consumed or altered by TU edits.
  bool DefinitionDirectiveTouchedByTUEdit(
      const RefoldModel::MacroDirective &Directive) const;
  /// Returns whether a physical macro invocation callsite already has a patch.
  bool PhysicalCallsiteAlreadyHasPatch(
      const RefoldModel::MacroInvocation &Invocation) const;
  /// Finds the TU-visible include site that owns a macro invocation, if any.
  const RefoldModel::IncludeItem *OwningIncludeSiteForInvocationInTU(
      const RefoldModel::MacroInvocation &Invocation) const;
  /// Returns whether an include-owned invocation survives the final TU edits.
  bool IncludeOwnedInvocationSurvivesTUEdits(
      const RefoldModel::MacroInvocation &Invocation) const;
  /// Returns whether an invocation callsite remains present after final edits.
  bool InvocationCallsiteSurvivesTUEdits(
      const RefoldModel::MacroInvocation &Invocation) const;
  /// Finds a conservative fallback active definition for an invocation.
  const RefoldModel::MacroDirective *FallbackActiveDefinitionForInvocation(
      const RefoldModel::MacroInvocation &Invocation) const;
  /// Finds the active macro definition that should govern an invocation.
  const RefoldModel::MacroDirective *ActiveDefinitionForInvocation(
      const RefoldModel::MacroInvocation &Invocation) const;
  /// Returns whether another same-name transition survives and can preserve the
  /// required macro-state boundary.
  bool DefinitionHasOtherSurvivingSameNameTransition(
      const RefoldModel::MacroDirective &Definition, StringRef MacroName) const;
  /// Finds the previous live definition before an undef directive.
  const RefoldModel::MacroDirective *PreviousLiveDefinitionBeforeDirective(
      const NamedMacroDirectiveRef &UndefRef) const;
  /// Returns whether a macro directive remains represented after TU edits.
  bool DirectiveSurvivesTUEdits(
      const RefoldModel::MacroDirective &Directive) const;

  /// Returns whether a materialized include subtree owns a macro directive.
  bool MaterializedIncludeSubtreeOwnsMacroDirective(
      const RefoldModel::IncludeItem &Root,
      const RefoldModel::MacroDirective &Directive) const;
  /// Returns whether a materialized replacement preserves the include ancestry
  /// required for a macro directive.
  bool MaterializedReplacementPreservesDirectiveAncestry(
      const RefoldModel::IncludeItem &Root,
      const RefoldModel::MacroDirective &Directive,
      const TextEdit &ReplacementEdit) const;
  /// Returns whether an invocation still survives after the materialized include
  /// replacement site.
  bool InvocationSurvivesAfterMaterializedInclude(
      const RefoldModel::MacroInvocation &Invocation,
      const RefoldModel::IncludeItem &MaterializedInclude,
      uint64_t MaterializedSiteEnd) const;
  /// Returns whether a materialized include must carry a definition afterward to
  /// satisfy surviving downstream observers.
  bool MaterializedIncludeNeedsDefinitionAfterward(
      const RefoldModel::IncludeItem &MaterializedInclude,
      uint64_t MaterializedSiteEnd,
      const RefoldModel::MacroDirective &Definition) const;

  const Dependencies &deps_;
  MacroStateRepairPlan &plan_;
  RefoldStructuralHunkDispatcher &dispatcher_;
  std::vector<TextEdit> &tuEdits_;
  StringRef tuPath_;
  StringRef tuBytes_;
  std::map<size_t, SmallVector<MacroStatePreservation, 4>>
      macroStatePreservationsByEdit_;
};

void MacroStateRepairContext::BuildDirectiveIndexOnce() {
  if (plan_.DirectiveIndexBuilt)
    return;

  plan_.MacroDirectiveById.clear();
  plan_.NamedMacroDirectives.clear();

  // Build a name-indexed view of macro-state directives that can affect later
  // preserved source.  The producer records the controlled #define/#undef macro
  // name directly, so this proof never reparses directive text merely to recover
  // the macro-state key.  Empty names are malformed proof data and ignored
  // fail-closed.
  for (const RefoldModel::MacroDirective &Directive : Model().GetMacroDirectives()) {
    if (Directive.subkind != "#define" && Directive.subkind != "#undef")
      continue;
    if (Directive.name.empty())
      continue;
    NamedMacroDirectiveRef Ref{&Directive, Directive.name};
    plan_.MacroDirectiveById[Directive.id] = Ref;
    plan_.NamedMacroDirectives.push_back(Ref);
  }

  // Keep the ordered view deterministic so macro-state damage intervals can be
  // computed by walking directives in their original source order.
  llvm::sort(plan_.NamedMacroDirectives,
             [](const NamedMacroDirectiveRef &LHS,
                const NamedMacroDirectiveRef &RHS) {
               return LHS.Directive->id < RHS.Directive->id;
             });

  plan_.DirectiveIndexBuilt = true;
}

bool MacroStateRepairContext::SourceIntervalsOverlap(uint64_t ABegin,
                                                     uint64_t AEnd,
                                                     uint64_t BBegin,
                                                     uint64_t BEnd) {
  return ABegin < BEnd && BBegin < AEnd;
}

bool MacroStateRepairContext::IntervalOverlapsFinalTUEdit(uint64_t Begin,
                                                          uint64_t End) const {
  for (const TextEdit &Edit : tuEdits_) {
    if (SourceIntervalsOverlap(Begin, End, Edit.start, Edit.end))
      return true;
  }
  return false;
}

std::optional<size_t>
MacroStateRepairContext::FinalTUEditContainingInterval(uint64_t Begin,
                                                       uint64_t End) const {
  for (size_t EditIndex = 0; EditIndex < tuEdits_.size(); ++EditIndex) {
    const TextEdit &Edit = tuEdits_[EditIndex];
    if (Edit.start <= Begin && End <= Edit.end)
      return EditIndex;
  }
  return std::nullopt;
}

bool MacroStateRepairContext::SourceRangeOverlapsFinalTUEditExcept(
    uint64_t Begin, uint64_t End, size_t ExceptEditIndex) const {
  for (size_t EditIndex = 0; EditIndex < tuEdits_.size(); ++EditIndex) {
    if (EditIndex == ExceptEditIndex)
      continue;
    const TextEdit &Edit = tuEdits_[EditIndex];
    if (SourceIntervalsOverlap(Begin, End, Edit.start, Edit.end))
      return true;
  }
  return false;
}

const RefoldModel::IncludeItem *
MacroStateRepairContext::OutermostOwningIncludeSiteInTU(
    std::optional<uint64_t> IncludeId) const {
  if (!IncludeId)
    return nullptr;

  const RefoldModel::IncludeItem *Cur = Model().GetIncludeById(*IncludeId);
  while (Cur) {
    if (PathIdentity().PathsEqual(Cur->sitePath, tuPath_))
      return Cur;
    if (!Cur->parent)
      return nullptr;
    Cur = Model().GetIncludeById(*Cur->parent);
  }
  return nullptr;
}

const RefoldModel::IncludeItem *
MacroStateRepairContext::OwningIncludeSiteInTU(
    const RefoldModel::MacroDirective &Directive) const {
  return OutermostOwningIncludeSiteInTU(Directive.ownerIncludeId);
}

StringRef MacroStateRepairContext::IncludeDirectiveText(
    const RefoldModel::IncludeItem &Inc) const {
  if (!Inc.text.empty())
    return Inc.text;
  if (PathIdentity().PathsEqual(Inc.sitePath, tuPath_) &&
      Inc.siteE <= tuBytes_.size())
    return tuBytes_.slice(Inc.siteB, Inc.siteE);
  return StringRef();
}

bool MacroStateRepairContext::IncludeDirectiveAppearsAtLineStart(
    const TextEdit &Edit, const RefoldModel::IncludeItem &Inc) const {
  StringRef DirectiveText = IncludeDirectiveText(Inc);
  return !DirectiveText.empty() &&
         stringutils::containsAtLineStartAfterIndent(Edit.text, DirectiveText);
}

bool MacroStateRepairContext::IncludeSitePreservedByFinalTUEdit(
    const RefoldModel::IncludeItem &Inc) const {
  std::optional<size_t> EditIndex =
      FinalTUEditContainingInterval(Inc.siteB, Inc.siteE);
  if (!EditIndex)
    return false;
  return IncludeDirectiveAppearsAtLineStart(tuEdits_[*EditIndex], Inc);
}

bool MacroStateRepairContext::IncludeAncestrySitePreservedByFinalTUEdit(
    const RefoldModel::MacroDirective &Directive) const {
  if (!Directive.ownerIncludeId)
    return false;

  const RefoldModel::IncludeItem *OuterTU =
      OutermostOwningIncludeSiteInTU(Directive.ownerIncludeId);
  if (!OuterTU)
    return false;

  std::optional<size_t> EditIndex =
      FinalTUEditContainingInterval(OuterTU->siteB, OuterTU->siteE);
  if (!EditIndex)
    return false;
  const TextEdit &Edit = tuEdits_[*EditIndex];

  // Header materialization may remove the outer TU #include while preserving a
  // descendant include directive in the materialized replacement text.  Walk
  // from the directive's immediate owning include toward the TU include; if any
  // include in that ancestry is carried forward as a directive line, then the
  // macro-state transition remains available through source order and must not
  // be hoisted into the TU.
  const RefoldModel::IncludeItem *Cur =
      Model().GetIncludeById(*Directive.ownerIncludeId);
  while (Cur) {
    if (IncludeDirectiveAppearsAtLineStart(Edit, *Cur))
      return true;
    if (!Cur->parent)
      break;
    Cur = Model().GetIncludeById(*Cur->parent);
  }
  return false;
}

std::optional<MacroDirectiveSourceInterval>
MacroStateRepairContext::MacroDirectiveFullSourceInterval(
    const RefoldModel::MacroDirective &Directive) const {
  return MacroStateProof().RecoverMacroStateDirectiveLineInterval(
      Directive, tuPath_, tuBytes_, std::nullopt);
}

bool MacroStateRepairContext::MacroStateDirectiveAppearsAtLineStart(
    const TextEdit &Edit, const RefoldModel::MacroDirective &Directive) const {
  return !Directive.text.empty() &&
         stringutils::containsAtLineStartAfterIndent(Edit.text, Directive.text);
}

std::optional<size_t>
MacroStateRepairContext::FinalTUEditContainingMacroDirective(
    const RefoldModel::MacroDirective &Directive) const {
  if (PathIdentity().PathsEqual(Directive.sitePath, tuPath_)) {
    std::optional<size_t> EditIndex =
        FinalTUEditContainingInterval(Directive.siteB, Directive.siteE);
    if (EditIndex &&
        MacroStateDirectiveAppearsAtLineStart(tuEdits_[*EditIndex], Directive))
      return std::nullopt;
    return EditIndex;
  }

  if (IncludeAncestrySitePreservedByFinalTUEdit(Directive))
    return std::nullopt;

  if (const RefoldModel::IncludeItem *Inc = OwningIncludeSiteInTU(Directive)) {
    if (IncludeSitePreservedByFinalTUEdit(*Inc))
      return std::nullopt;
    return FinalTUEditContainingInterval(Inc->siteB, Inc->siteE);
  }

  return std::nullopt;
}

bool MacroStateRepairContext::MacroDirectiveTouchedByTUEdit(
    const RefoldModel::MacroDirective &Directive) const {
  if (PathIdentity().PathsEqual(Directive.sitePath, tuPath_)) {
    for (const TextEdit &Edit : tuEdits_) {
      if (!SourceIntervalsOverlap(Directive.siteB, Directive.siteE, Edit.start,
                                  Edit.end))
        continue;
      return !MacroStateDirectiveAppearsAtLineStart(Edit, Directive);
    }
    return false;
  }

  if (IncludeAncestrySitePreservedByFinalTUEdit(Directive))
    return false;

  if (const RefoldModel::IncludeItem *Inc = OwningIncludeSiteInTU(Directive)) {
    if (IncludeSitePreservedByFinalTUEdit(*Inc))
      return false;
    return IntervalOverlapsFinalTUEdit(Inc->siteB, Inc->siteE);
  }

  return false;
}

std::string MacroStateRepairContext::DirectiveTextForPreservation(
    const RefoldModel::MacroDirective &Directive) const {
  std::string Text = Directive.text.str();
  if (Text.empty() || Text.back() != '\n')
    Text.push_back('\n');
  return Text;
}

std::optional<MacroStateSourceTransition>
MacroStateRepairContext::MacroStateSourceTransitionFor(
    const RefoldModel::MacroDirective &Directive) const {
  if (PathIdentity().PathsEqual(Directive.sitePath, tuPath_) &&
      !Directive.ownerIncludeId) {
    std::optional<MacroDirectiveSourceInterval> Interval =
        MacroDirectiveFullSourceInterval(Directive);
    if (!Interval)
      return std::nullopt;
    return MacroStateSourceTransition{*Interval,
                                      DirectiveTextForPreservation(Directive)};
  }

  const RefoldModel::IncludeItem *Inc = OwningIncludeSiteInTU(Directive);
  if (!Inc)
    return std::nullopt;
  StringRef Text = IncludeDirectiveText(*Inc);
  if (Text.empty())
    return std::nullopt;
  std::string Spelling = Text.str();
  if (Spelling.empty() || Spelling.back() != '\n')
    Spelling.push_back('\n');
  MacroDirectiveSourceInterval Interval;
  Interval.begin = Inc->siteB;
  Interval.end = Inc->siteE;
  return MacroStateSourceTransition{Interval, std::move(Spelling)};
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::ActiveDefinitionAtSourceOffset(StringRef MacroName,
                                                        uint64_t Offset) const {
  const RefoldModel::MacroDirective *Active = nullptr;
  uint64_t ActiveEnd = 0;
  for (const NamedMacroDirectiveRef &Ref : plan_.NamedMacroDirectives) {
    const RefoldModel::MacroDirective &Candidate = *Ref.Directive;
    if (StringRef(Ref.Name) != MacroName)
      continue;
    std::optional<MacroStateSourceTransition> Transition =
        MacroStateSourceTransitionFor(Candidate);
    if (!Transition || Transition->Interval.end > Offset)
      continue;
    if (!Active || Transition->Interval.end > ActiveEnd ||
        (Transition->Interval.end == ActiveEnd && Candidate.id > Active->id)) {
      Active = &Candidate;
      ActiveEnd = Transition->Interval.end;
    }
  }
  return Active && Active->subkind == "#define" ? Active : nullptr;
}

bool MacroStateRepairContext::MacroStateDirectiveCanBeDelayedAfterEdit(
    const TextEdit &Edit, const RefoldModel::MacroDirective &Directive) const {
  if (PathIdentity().PathsEqual(Directive.sitePath, tuPath_) &&
      !Directive.ownerIncludeId) {
    std::optional<MacroDirectiveSourceInterval> DirectiveInterval =
        MacroDirectiveFullSourceInterval(Directive);
    return DirectiveInterval && Edit.start <= DirectiveInterval->begin &&
           DirectiveInterval->end <= Edit.end;
  }

  const RefoldModel::IncludeItem *Inc = OwningIncludeSiteInTU(Directive);
  if (!Inc)
    return false;
  if (Inc->siteB < Edit.start || Edit.end < Inc->siteE)
    return false;

  // If the replacement carries the include directive itself, the header
  // transition remains available through ordinary source order; hoisting the
  // header's macro-state directive into the TU would duplicate it.
  return !IncludeDirectiveAppearsAtLineStart(Edit, *Inc);
}

bool MacroStateRepairContext::MacroStateDefinitionAvailableBeforeSourceOffset(
    const RefoldModel::MacroDirective &Definition, uint64_t Offset) const {
  if (Definition.subkind != "#define")
    return false;

  if (PathIdentity().PathsEqual(Definition.sitePath, tuPath_) &&
      !Definition.ownerIncludeId) {
    std::optional<MacroDirectiveSourceInterval> Interval =
        MacroDirectiveFullSourceInterval(Definition);
    return Interval && Interval->end <= Offset;
  }

  const RefoldModel::IncludeItem *Inc = OwningIncludeSiteInTU(Definition);
  return Inc && Inc->siteE <= Offset;
}

std::optional<size_t>
MacroStateRepairContext::FirstReplacementObservationOffset(
    const TextEdit &Edit, const RefoldModel::MacroDirective &Definition,
    StringRef MacroName) const {
  return MacroStateProof().FirstMacroStateObservationOffsetInText(
      Definition, MacroName, StringRef(Edit.text), tuBytes_.drop_front(Edit.end));
}

bool MacroStateRepairContext::ReplacementObservesPreservedDefinition(
    const TextEdit &Edit, const RefoldModel::MacroDirective &Definition,
    StringRef MacroName) const {
  return FirstReplacementObservationOffset(Edit, Definition, MacroName)
      .has_value();
}

std::optional<size_t>
MacroStateRepairContext::ReplacementLineStartBeforeObservation(
    StringRef Replacement, size_t ObservationOffset) const {
  if (ObservationOffset > Replacement.size())
    return std::nullopt;

  for (size_t I = ObservationOffset; I > 0; --I) {
    const size_t Newline = I - 1;
    if (Replacement[Newline] != '\n')
      continue;
    if (stringutils::isLineSplice(Replacement, Newline))
      return std::nullopt;
    return Newline + 1;
  }
  return std::nullopt;
}

std::optional<size_t>
MacroStateRepairContext::ReplacementLineStartAfterFinalObservation(
    const TextEdit &Edit, const RefoldModel::MacroDirective &Definition,
    StringRef MacroName, size_t FirstObservationOffset) const {
  StringRef Replacement(Edit.text);
  if (FirstObservationOffset >= Replacement.size())
    return std::nullopt;

  for (size_t I = FirstObservationOffset + 1; I < Replacement.size(); ++I) {
    const size_t Newline = I - 1;
    if (Replacement[Newline] != '\n')
      continue;
    if (stringutils::isLineSplice(Replacement, Newline))
      continue;

    // Re-run the ordinary observation proof on the replacement suffix that would
    // appear after the inserted directive.  If that suffix has no observing
    // token, every replacement byte that must remain in B's pre-definition macro
    // state stays before the insertion point.
    TextEdit SuffixEdit = Edit;
    SuffixEdit.text = Replacement.drop_front(I).str();
    if (!FirstReplacementObservationOffset(SuffixEdit, Definition, MacroName))
      return I;
  }

  return std::nullopt;
}

bool MacroStateRepairContext::ReplacementSuffixBoundaryAllowsDirectiveLine(
    const TextEdit &Edit) const {
  if (Edit.end > tuBytes_.size())
    return false;

  StringRef Text(Edit.text);
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
  if (Edit.end >= tuBytes_.size() || stringutils::isWs(tuBytes_[Edit.end]))
    return true;

  std::optional<RefoldLexBoundaryToken> LeftTok =
      refoldLastLexToken(Text, LexLang());
  std::optional<RefoldLexBoundaryToken> RightTok =
      refoldFirstLexToken(tuBytes_.drop_front(Edit.end), LexLang());
  if (!LeftTok || !RightTok)
    return true;

  return !refoldNeedsLexicalSeparator(*LeftTok, *RightTok, LexLang());
}

bool MacroStateRepairContext::EditHasMacroPatchSurfaceInBMacroState(
    const TextEdit &Edit) const {
  for (const auto &Carrier : Edit.acceptedResults) {
    if (!Carrier || Carrier->kind != AcceptedResultCandidateKind::MacroPatch)
      continue;

    const ProofSummary &Summary = Carrier->proofSummary;
    if ((Summary.theoremClass == TheoremProofClass::OwnerRealizationProof ||
         Summary.theoremClass == TheoremProofClass::MixedOwnerTilingProof) &&
        Summary.inventory.currentPath ==
            AcceptedPathKind::MacroWholeCoverRealization &&
        Summary.realizationMode == RealizationMode::RealizeEditedSurface &&
        Summary.surfaceDisposition == SurfaceDisposition::RealizeWholeCoverMacros)
      return true;
  }
  return false;
}

OwnerStateBoundary MacroStateRepairContext::MacroDirectiveSuffixBoundary(
    const RefoldModel::MacroDirective &Directive) const {
  return OwnerStateBoundary::FromSource(OwnerSourceRange::From(
      Directive.sitePath, Directive.siteB, Directive.siteE,
      Directive.ownerIncludeId));
}

StateTransitionProof MacroStateRepairContext::CheckMacroStateWithWitness(
    const OwnerStateBoundary &Boundary, StateMutationKind Mutation,
    SuffixStabilityWitness Witness, StringRef Stage, StringRef Detail,
    bool RequireKnownObserver) const {
  return OwnerStateProof().CheckStateTransitionAcrossEditBoundary(
      Boundary, OwnerStateComponent::MacroState, Mutation, std::move(Witness),
      Stage, Detail, RequireKnownObserver);
}

StateTransitionProof MacroStateRepairContext::CheckMacroStateRepaired(
    const RefoldModel::MacroDirective &Directive, StateMutationKind Mutation,
    StringRef Stage, StringRef Detail, bool RequireKnownObserver) const {
  const OwnerStateBoundary Boundary = MacroDirectiveSuffixBoundary(Directive);
  return CheckMacroStateWithWitness(
      Boundary, Mutation,
      OwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::StateRepair,
          OwnerStateComponent::MacroState, Boundary, Detail),
      Stage, Detail, RequireKnownObserver);
}

StateTransitionProof MacroStateRepairContext::CheckMacroStateMaterialized(
    const RefoldModel::MacroDirective &Directive, StateMutationKind Mutation,
    StringRef Stage, StringRef Detail, bool RequireKnownObserver) const {
  const OwnerStateBoundary Boundary = MacroDirectiveSuffixBoundary(Directive);
  return CheckMacroStateWithWitness(
      Boundary, Mutation,
      OwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::OwnerMaterialization,
          OwnerStateComponent::MacroState, Boundary, Detail),
      Stage, Detail, RequireKnownObserver);
}

StateTransitionProof MacroStateRepairContext::CheckMacroStateTerminal(
    const RefoldModel::MacroDirective &Directive, StateMutationKind Mutation,
    StringRef Stage, StringRef Detail, bool RequireKnownObserver) const {
  const OwnerStateBoundary Boundary = MacroDirectiveSuffixBoundary(Directive);
  return CheckMacroStateWithWitness(
      Boundary, Mutation,
      OwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::TerminalStateFailure,
          OwnerStateComponent::MacroState, Boundary, Detail),
      Stage, Detail, RequireKnownObserver);
}

StateMutationKind
MacroStateRepairContext::MutationForMacroStatePreservationPlacement(
    MacroStatePreservationPlacement Placement) const {
  switch (Placement) {
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
    MacroStatePreservationPlacement Placement) const {
  switch (Placement) {
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

void MacroStateRepairContext::AttachConservativeTUCarrier(TextEdit &Edit) {
  TextEditAssembler().AttachAcceptedResultCarrier(
      Edit, ProofLattice().BuildAcceptedTUTextEditCandidate(
                AcceptedPathKind::TUByteSpanConservativeEdit, Edit.start,
                Edit.end, StringRef(Edit.text)));
}

void MacroStateRepairContext::RestageConservativeTUEdit(TextEdit &Edit,
                                                        uint64_t Start,
                                                        uint64_t End,
                                                        StringRef Replacement) {
  ResyncOutcome Resync = TextEditAssembler().ApplyResyncOrPend(
      tuBytes_, Start, End, Replacement, tuPath_);
  Edit.start = Start;
  Edit.end = End;
  Edit.text = std::move(Resync.text);
  Edit.pending = std::move(Resync.pending);
  Edit.lineControlPruneCandidates = std::move(Resync.lineControlPruneCandidates);

  // A repair edit is no longer the original direct TU hunk edit. It now carries
  // macro-state transition bytes around the original replacement.
  Edit.isDirectTUHunkEdit = false;
  Edit.directTUHunkIndex.reset();
  Edit.directTUHunkAStart.reset();
  Edit.directTUHunkAEnd.reset();
  Edit.directTUHunkBStart.reset();
  Edit.directTUHunkBEnd.reset();
  Edit.directTURawStart.reset();
  Edit.directTURawEnd.reset();
  Edit.directTUFinalStart = Edit.start;
  Edit.directTUFinalEnd = Edit.end;
  AttachConservativeTUCarrier(Edit);
}

std::optional<uint64_t>
MacroStateRepairContext::DelayedTransitionBoundaryAfterEdit(
    size_t EditIndex, const RefoldModel::MacroDirective &Definition,
    StringRef MacroName) const {
  if (EditIndex >= tuEdits_.size())
    return std::nullopt;
  const TextEdit &Edit = tuEdits_[EditIndex];
  if (Edit.end > tuBytes_.size())
    return std::nullopt;

  const size_t EditEnd = static_cast<size_t>(Edit.end);
  size_t LineEnd = stringutils::lineEndOffset(tuBytes_, EditEnd);

  // Lexical separability alone is not enough for natural directive placement.
  // Splitting `int x = M + 2;` as payload / directive / `;` preserves tokens but
  // moves a directive into a physical declaration.  Only use the edit/suffix
  // boundary directly when it is already at physical line end; otherwise try to
  // absorb the neutral rest of that source line.
  if (EditEnd == LineEnd && ReplacementSuffixBoundaryAllowsDirectiveLine(Edit))
    return Edit.end;

  if (LineEnd <= EditEnd)
    return std::nullopt;
  if (LineEnd < tuBytes_.size()) {
    if (stringutils::isLineSplice(tuBytes_, LineEnd))
      return std::nullopt;
    ++LineEnd;
  }

  if (SourceRangeOverlapsFinalTUEditExcept(Edit.end, LineEnd, EditIndex))
    return std::nullopt;

  StringRef CarriedSuffix = tuBytes_.slice(Edit.end, LineEnd);
  if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
          Definition, MacroName, CarriedSuffix, tuBytes_.drop_front(LineEnd)))
    return std::nullopt;

  TextEdit BoundaryEdit = Edit;
  BoundaryEdit.end = LineEnd;
  BoundaryEdit.text.append(CarriedSuffix.begin(), CarriedSuffix.end());
  if (!ReplacementSuffixBoundaryAllowsDirectiveLine(BoundaryEdit))
    return std::nullopt;

  return static_cast<uint64_t>(LineEnd);
}

void MacroStateRepairContext::AdvancePreservedUndefsBeforeObservedReplacements() {
  SmallVector<size_t, 16> EditOrder;
  EditOrder.reserve(tuEdits_.size());
  for (size_t EditIndex = 0; EditIndex < tuEdits_.size(); ++EditIndex)
    EditOrder.push_back(EditIndex);
  llvm::sort(EditOrder, [&](size_t LHS, size_t RHS) {
    if (tuEdits_[LHS].start != tuEdits_[RHS].start)
      return tuEdits_[LHS].start < tuEdits_[RHS].start;
    return LHS < RHS;
  });

  DenseSet<uint64_t> AdvancedDirectiveIds;
  size_t AdvancedCount = 0;

  for (size_t EditIndex : EditOrder) {
    if (EditIndex >= tuEdits_.size())
      continue;
    TextEdit &Edit = tuEdits_[EditIndex];
    if (Edit.start > Edit.end || Edit.end > tuBytes_.size())
      continue;

    const size_t EditStart = static_cast<size_t>(Edit.start);
    const size_t LineStart = stringutils::lineStartOffset(tuBytes_, EditStart);
    if (LineStart >= EditStart)
      continue;
    if (LineStart > 0 && stringutils::isLineSplice(tuBytes_, LineStart - 1))
      continue;
    if (SourceRangeOverlapsFinalTUEditExcept(LineStart, Edit.start, EditIndex))
      continue;

    StringRef CrossedPrefix = tuBytes_.slice(LineStart, Edit.start);
    StringRef ReplacementText(Edit.text);

    for (const NamedMacroDirectiveRef &UndefRef : plan_.NamedMacroDirectives) {
      const RefoldModel::MacroDirective &UndefDirective = *UndefRef.Directive;
      if (UndefDirective.subkind != "#undef")
        continue;
      if (AdvancedDirectiveIds.contains(UndefDirective.id))
        continue;

      std::optional<MacroStateSourceTransition> UndefTransition =
          MacroStateSourceTransitionFor(UndefDirective);
      if (!UndefTransition)
        continue;

      // This pass only advances a preserved future transition. If the transition
      // begins inside the current edit, it is a consumed-#undef case handled by
      // the consumed-transition repair instead.
      if (UndefTransition->Interval.begin < Edit.end)
        continue;

      const RefoldModel::MacroDirective *PreviousDefinition =
          ActiveDefinitionAtSourceOffset(UndefRef.Name, LineStart);
      if (!PreviousDefinition)
        continue;

      std::optional<size_t> FirstObservationOffset =
          FirstReplacementObservationOffset(Edit, *PreviousDefinition,
                                            UndefRef.Name);
      if (!FirstObservationOffset)
        continue;

      if (SourceRangeOverlapsFinalTUEditExcept(
              LineStart, UndefTransition->Interval.end, EditIndex))
        continue;

      StringRef ReplacementPrefix =
          ReplacementText.take_front(*FirstObservationOffset);
      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *PreviousDefinition, UndefRef.Name, ReplacementPrefix,
              ReplacementText.drop_front(*FirstObservationOffset)))
        continue;

      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *PreviousDefinition, UndefRef.Name, CrossedPrefix,
              ReplacementText))
        continue;

      StringRef CarriedSuffix =
          tuBytes_.slice(Edit.end, UndefTransition->Interval.begin);
      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              *PreviousDefinition, UndefRef.Name, CarriedSuffix,
              tuBytes_.drop_front(UndefTransition->Interval.begin)))
        continue;

      std::string Replacement;
      Replacement.reserve(UndefTransition->Text.size() + CrossedPrefix.size() +
                          ReplacementText.size() + CarriedSuffix.size());
      Replacement += UndefTransition->Text;
      Replacement.append(CrossedPrefix.begin(), CrossedPrefix.end());
      Replacement.append(ReplacementText.begin(), ReplacementText.end());
      Replacement.append(CarriedSuffix.begin(), CarriedSuffix.end());

      const uint64_t OldStart = Edit.start;
      const uint64_t OldEnd = Edit.end;
      RestageConservativeTUEdit(Edit, LineStart, UndefTransition->Interval.end,
                                Replacement);

      (void)CheckMacroStateRepaired(
          UndefDirective, StateMutationKind::MovedEarlier,
          "macro-undef-liveness",
          llvm::formatv("advanced preserved #undef directive #{0} for macro "
                        "'{1}' before observing replacement [{2},{3})",
                        UndefDirective.id, UndefRef.Name, OldStart, OldEnd)
              .str());

      AdvancedDirectiveIds.insert(UndefDirective.id);
      ++AdvancedCount;
      REFOLD_LOG_WARN("macro/liveness",
                      "advancing preserved #undef before observed replacement: "
                      "macro='{0}' undefDirective=#{1} priorDefine=#{2} "
                      "edit=[{3},{4}) widened=[{5},{6})",
                      UndefRef.Name, UndefDirective.id, PreviousDefinition->id,
                      OldStart, OldEnd, Edit.start, Edit.end);
      break;
    }
  }

  if (AdvancedCount != 0) {
    REFOLD_LOG_INFO("macro/liveness",
                    "advanced {0} preserved #undef directive(s) before "
                    "replacement payloads to keep edited tokens in B macro state",
                    AdvancedCount);
  }
}

std::optional<MacroStatePreservationPlacement>
MacroStateRepairContext::TryAdvanceConsumedUndefBeforeObservedReplacement(
    size_t EditIndex, const RefoldModel::MacroDirective &UndefDirective,
    StringRef MacroName, const RefoldModel::MacroDirective &PreviousDefinition,
    size_t FirstObservationOffset) {
  if (EditIndex >= tuEdits_.size())
    return std::nullopt;

  TextEdit &Edit = tuEdits_[EditIndex];
  if (Edit.start == 0 || Edit.start > tuBytes_.size())
    return std::nullopt;

  const size_t EditStart = static_cast<size_t>(Edit.start);
  const size_t LineStart = stringutils::lineStartOffset(tuBytes_, EditStart);
  if (LineStart >= EditStart)
    return std::nullopt;

  if (LineStart > 0 && stringutils::isLineSplice(tuBytes_, LineStart - 1))
    return std::nullopt;

  if (!MacroStateDefinitionAvailableBeforeSourceOffset(PreviousDefinition,
                                                       LineStart))
    return std::nullopt;

  if (SourceRangeOverlapsFinalTUEditExcept(LineStart, Edit.start, EditIndex))
    return std::nullopt;

  StringRef ReplacementText(Edit.text);
  if (FirstObservationOffset > ReplacementText.size())
    return std::nullopt;

  StringRef ReplacementPrefix = ReplacementText.take_front(FirstObservationOffset);
  if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
          PreviousDefinition, MacroName, ReplacementPrefix,
          ReplacementText.drop_front(FirstObservationOffset)))
    return std::nullopt;

  StringRef CrossedPrefix = tuBytes_.slice(LineStart, Edit.start);
  if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
          PreviousDefinition, MacroName, CrossedPrefix, ReplacementText))
    return std::nullopt;

  std::string Replacement;
  const std::string DirectiveText = DirectiveTextForPreservation(UndefDirective);
  Replacement.reserve(DirectiveText.size() + CrossedPrefix.size() +
                      ReplacementText.size());
  Replacement += DirectiveText;
  Replacement.append(CrossedPrefix.begin(), CrossedPrefix.end());
  Replacement.append(ReplacementText.begin(), ReplacementText.end());

  const uint64_t OldStart = Edit.start;
  RestageConservativeTUEdit(Edit, LineStart, Edit.end, Replacement);

  REFOLD_LOG_WARN("macro/liveness",
                  "advancing consumed #undef before observed replacement: "
                  "macro='{0}' undefDirective=#{1} priorDefine=#{2} "
                  "edit=[{3},{4}) widenedStart={5}",
                  MacroName, UndefDirective.id, PreviousDefinition.id, OldStart,
                  Edit.end, Edit.start);

  return MacroStatePreservationPlacement::AdvancedBeforeReplacement;
}

bool MacroStateRepairContext::TryWidenEditToDelayedMacroStateBoundary(
    size_t EditIndex, const RefoldModel::MacroDirective &Definition,
    StringRef MacroName) {
  if (EditIndex >= tuEdits_.size())
    return false;

  TextEdit &Edit = tuEdits_[EditIndex];
  std::optional<uint64_t> Boundary =
      DelayedTransitionBoundaryAfterEdit(EditIndex, Definition, MacroName);
  if (!Boundary)
    return false;
  if (*Boundary == Edit.end)
    return true;
  if (*Boundary < Edit.end || *Boundary > tuBytes_.size())
    return false;

  std::string Replacement = Edit.text;
  Replacement.append(tuBytes_.begin() + Edit.end, tuBytes_.begin() + *Boundary);

  const uint64_t OldEnd = Edit.end;
  RestageConservativeTUEdit(Edit, Edit.start, *Boundary, Replacement);

  REFOLD_LOG_TRACE("macro/liveness",
                   "widened TU edit to delayed macro-state boundary: "
                   "macro='{0}' definitionDirective=#{1} oldEnd={2} newEnd={3}",
                   MacroName, Definition.id, OldEnd, Edit.end);
  return true;
}

std::optional<MacroStatePreservationPlacement>
MacroStateRepairContext::TryQueueMacroStateDirectivePreservation(
    size_t EditIndex, const RefoldModel::MacroDirective &Directive,
    StringRef MacroName,
    const RefoldModel::MacroDirective *ObservedDefinition) {
  if (EditIndex >= tuEdits_.size())
    return std::nullopt;

  TextEdit &Edit = tuEdits_[EditIndex];

  const RefoldModel::MacroDirective *DefinitionObservedByReplacement =
      ObservedDefinition
          ? ObservedDefinition
          : (Directive.subkind == "#define" ? &Directive : nullptr);
  const std::optional<size_t> FirstObservationOffset =
      DefinitionObservedByReplacement
          ? FirstReplacementObservationOffset(
                Edit, *DefinitionObservedByReplacement, MacroName)
          : TokenTextAnalysis().FirstRawIdentifierObservationOffsetInText(
                MacroName, StringRef(Edit.text));
  const bool ReplacementObservesDefinition = FirstObservationOffset.has_value();

  const bool EditStartsAtPhysicalBOL =
      Edit.start == 0 || tuBytes_[Edit.start - 1] == '\n';
  const bool BeforePlacementWouldExposeDefine =
      Directive.subkind == "#define" && ReplacementObservesDefinition;
  if (EditStartsAtPhysicalBOL && !BeforePlacementWouldExposeDefine) {
    macroStatePreservationsByEdit_[EditIndex].push_back(MacroStatePreservation{
        &Directive, MacroStatePreservationPlacement::BeforeReplacement});
    return MacroStatePreservationPlacement::BeforeReplacement;
  }

  if (!MacroStateDirectiveCanBeDelayedAfterEdit(Edit, Directive))
    return std::nullopt;

  if (Directive.subkind == "#undef" && ReplacementObservesDefinition) {
    if (!FirstObservationOffset || !DefinitionObservedByReplacement)
      return std::nullopt;
    std::optional<size_t> InsertionOffset =
        ReplacementLineStartBeforeObservation(StringRef(Edit.text),
                                              *FirstObservationOffset);
    if (InsertionOffset) {
      macroStatePreservationsByEdit_[EditIndex].push_back(
          MacroStatePreservation{&Directive,
                                 MacroStatePreservationPlacement::InsideReplacement,
                                 *InsertionOffset});
      return MacroStatePreservationPlacement::InsideReplacement;
    }

    return TryAdvanceConsumedUndefBeforeObservedReplacement(
        EditIndex, Directive, MacroName, *DefinitionObservedByReplacement,
        *FirstObservationOffset);
  }

  if (Directive.subkind == "#define" && ReplacementObservesDefinition &&
      DefinitionObservedByReplacement) {
    if (!FirstObservationOffset)
      return std::nullopt;
    std::optional<size_t> InsertionOffset =
        ReplacementLineStartAfterFinalObservation(Edit, *DefinitionObservedByReplacement,
                                                  MacroName,
                                                  *FirstObservationOffset);
    if (InsertionOffset) {
      macroStatePreservationsByEdit_[EditIndex].push_back(
          MacroStatePreservation{&Directive,
                                 MacroStatePreservationPlacement::InsideReplacement,
                                 *InsertionOffset});
      return MacroStatePreservationPlacement::InsideReplacement;
    }
  }

  if (Directive.subkind == "#undef" && !ReplacementObservesDefinition &&
      DefinitionObservedByReplacement) {
    (void)TryWidenEditToDelayedMacroStateBoundary(
        EditIndex, *DefinitionObservedByReplacement, MacroName);
  }

  if (!ReplacementSuffixBoundaryAllowsDirectiveLine(Edit))
    return std::nullopt;

  macroStatePreservationsByEdit_[EditIndex].push_back(MacroStatePreservation{
      &Directive, MacroStatePreservationPlacement::AfterReplacement});
  return MacroStatePreservationPlacement::AfterReplacement;
}

// Insert a synthetic #undef at the start of a widened replacement line when
// the preserved source before the edit intentionally crosses a definition, but
// the replacement text must observe B's undefined macro state.  This is a
// conservative token-level repair: it does not attempt to restore the
// definition later, so suffix text that still needs the definition fails closed.
void MacroStateRepairContext::SynthesizeUndefBeforeObservedGapDefinitions() {
  SmallVector<size_t, 16> EditOrder;
  EditOrder.reserve(tuEdits_.size());
  for (size_t EditIndex = 0; EditIndex < tuEdits_.size(); ++EditIndex)
    EditOrder.push_back(EditIndex);
  llvm::sort(EditOrder, [&](size_t LHS, size_t RHS) {
    if (tuEdits_[LHS].start != tuEdits_[RHS].start)
      return tuEdits_[LHS].start < tuEdits_[RHS].start;
    return LHS < RHS;
  });

  size_t SynthesizedCount = 0;
  for (size_t EditIndex : EditOrder) {
    if (EditIndex >= tuEdits_.size())
      continue;
    TextEdit &Edit = tuEdits_[EditIndex];
    if (Edit.start > Edit.end || Edit.end > tuBytes_.size())
      continue;

    const size_t EditStart = static_cast<size_t>(Edit.start);
    const size_t LineStart = stringutils::lineStartOffset(tuBytes_, EditStart);
    if (LineStart > EditStart)
      continue;
    if (LineStart > 0 && stringutils::isLineSplice(tuBytes_, LineStart - 1))
      continue;
    if (SourceRangeOverlapsFinalTUEditExcept(LineStart, Edit.start, EditIndex))
      continue;

    StringRef SameLinePrefix = tuBytes_.slice(LineStart, Edit.start);
    StringRef ReplacementText(Edit.text);
    StringRef UntouchedSuffix = tuBytes_.drop_front(Edit.end);

    SmallVector<SyntheticUndefCandidate, 4> Candidates;
    for (const NamedMacroDirectiveRef &Ref : plan_.NamedMacroDirectives) {
      const RefoldModel::MacroDirective &Definition = *Ref.Directive;
      if (Definition.subkind != "#define")
        continue;
      if (plan_.SyntheticUndefPartitionedDefinitionIds.contains(Definition.id))
        continue;

      std::optional<MacroStateSourceTransition> Transition =
          MacroStateSourceTransitionFor(Definition);
      if (!Transition)
        continue;
      if (Transition->Interval.end > LineStart)
        continue;
      if (ActiveDefinitionAtSourceOffset(Ref.Name, LineStart) != &Definition)
        continue;

      std::optional<size_t> FirstObservationOffset =
          FirstReplacementObservationOffset(Edit, Definition, Ref.Name);
      if (!FirstObservationOffset)
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
              ReplacementText.take_front(*FirstObservationOffset + 1)))
        continue;

      // If the original same-line prefix needed the definition, placing a
      // synthetic #undef before the line would change preserved source before
      // the replacement.
      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              Definition, Ref.Name, SameLinePrefix, ReplacementText))
        continue;

      // Use this partition only when the crossed pre-edit region really is a
      // macro-state barrier/observer.  If it is neutral, the existing carry
      // proof can move the definition after the replacement without adding a
      // synthetic transition.
      if (!MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              Definition, Ref.Name,
              tuBytes_.slice(Transition->Interval.end, Edit.start),
              ReplacementText))
        continue;

      // This minimal partition does not restore the definition after the
      // replacement.  Do not synthesize it when later preserved source would
      // observe the old definition; that requires an explicit restore tiling.
      if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              Definition, Ref.Name, UntouchedSuffix, StringRef()))
        continue;

      Candidates.push_back(SyntheticUndefCandidate{
          &Definition, Transition->Interval, Ref.Name.str()});
    }

    if (Candidates.empty())
      continue;

    llvm::sort(Candidates,
               [](const SyntheticUndefCandidate &LHS,
                  const SyntheticUndefCandidate &RHS) {
                 if (LHS.Interval.begin != RHS.Interval.begin)
                   return LHS.Interval.begin < RHS.Interval.begin;
                 return LHS.Definition->id < RHS.Definition->id;
               });

    std::string UndefPrefix;
    for (const SyntheticUndefCandidate &Candidate : Candidates) {
      UndefPrefix += "#undef ";
      UndefPrefix += Candidate.Name;
      UndefPrefix.push_back('\n');
    }

    std::string Replacement;
    Replacement.reserve(UndefPrefix.size() + SameLinePrefix.size() +
                        ReplacementText.size());
    Replacement += UndefPrefix;
    Replacement.append(SameLinePrefix.begin(), SameLinePrefix.end());
    Replacement.append(ReplacementText.begin(), ReplacementText.end());

    const uint64_t OldStart = Edit.start;
    const uint64_t OldEnd = Edit.end;
    RestageConservativeTUEdit(Edit, LineStart, OldEnd, Replacement);

    for (const SyntheticUndefCandidate &Candidate : Candidates) {
      plan_.SyntheticUndefPartitionedDefinitionIds.insert(
          Candidate.Definition->id);
      (void)CheckMacroStateRepaired(
          *Candidate.Definition, StateMutationKind::MovedEarlier,
          "macro-synthetic-undef-partition",
          llvm::formatv("synthesized #undef for active definition #{0} of "
                        "macro '{1}' before observing TU replacement [{2},{3})",
                        Candidate.Definition->id, Candidate.Name, OldStart,
                        OldEnd)
              .str());
      ++SynthesizedCount;
    }

    REFOLD_LOG_WARN("macro/liveness",
                    "synthesizing local #undef partition before observed "
                    "replacement: defs={0} edit=[{1},{2}) widened=[{3},{4})",
                    Candidates.size(), OldStart, OldEnd, Edit.start, Edit.end);
  }

  if (SynthesizedCount != 0)
    REFOLD_LOG_INFO("macro/liveness",
                    "synthesized {0} local #undef partition(s) before observing "
                    "replacement payloads",
                    SynthesizedCount);
}

void MacroStateRepairContext::CarryObservedGapDefinitionsAfterReplacements() {
  SmallVector<size_t, 16> EditOrder;
  EditOrder.reserve(tuEdits_.size());
  for (size_t EditIndex = 0; EditIndex < tuEdits_.size(); ++EditIndex)
    EditOrder.push_back(EditIndex);
  llvm::sort(EditOrder, [&](size_t LHS, size_t RHS) {
    if (tuEdits_[LHS].start != tuEdits_[RHS].start)
      return tuEdits_[LHS].start < tuEdits_[RHS].start;
    return LHS < RHS;
  });

  DenseSet<uint64_t> CarriedDirectiveIds;
  size_t CarriedCount = 0;

  for (size_t EditIndex : EditOrder) {
    if (EditIndex >= tuEdits_.size())
      continue;
    TextEdit &Edit = tuEdits_[EditIndex];
    if (Edit.start > Edit.end || Edit.end > tuBytes_.size())
      continue;

    std::optional<uint64_t> DelayedBoundary;
    SmallVector<MacroStateGapCarryCandidate, 4> Candidates;
    for (const NamedMacroDirectiveRef &Ref : plan_.NamedMacroDirectives) {
      const RefoldModel::MacroDirective &Directive = *Ref.Directive;
      if (Directive.subkind != "#define")
        continue;
      if (CarriedDirectiveIds.contains(Directive.id))
        continue;
      if (plan_.SyntheticUndefPartitionedDefinitionIds.contains(Directive.id))
        continue;

      std::optional<MacroStateSourceTransition> Transition =
          MacroStateSourceTransitionFor(Directive);
      if (!Transition)
        continue;
      if (Transition->Interval.end > Edit.start)
        continue;
      if (IntervalOverlapsFinalTUEdit(Transition->Interval.begin,
                                      Transition->Interval.end))
        continue;
      if (ActiveDefinitionAtSourceOffset(Ref.Name, Edit.start) != &Directive)
        continue;
      if (!ReplacementObservesPreservedDefinition(Edit, Directive, Ref.Name))
        continue;

      std::optional<uint64_t> Boundary =
          DelayedTransitionBoundaryAfterEdit(EditIndex, Directive, Ref.Name);
      if (!Boundary)
        continue;
      if (DelayedBoundary && *DelayedBoundary != *Boundary)
        continue;
      DelayedBoundary = Boundary;

      // Only carry a gap definition when the B-side replacement is the first
      // material that would observe the definition.  Macro-callsite edits whose
      // emitted surface is kept in B's macro state are the important exception:
      // the original source slice is itself the macro invocation, so it
      // necessarily observes the old definition, while the accepted macro proof
      // says the replacement wants to keep the emitted callsite surface under B's
      // macro state.
      if (!EditHasMacroPatchSurfaceInBMacroState(Edit) &&
          MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
              Directive, Ref.Name, tuBytes_.slice(Edit.start, Edit.end),
              tuBytes_.drop_front(Edit.end)))
        continue;

      Candidates.push_back(MacroStateGapCarryCandidate{
          &Directive, Transition->Interval, std::move(Transition->Text),
          Ref.Name.str()});
    }

    if (Candidates.empty() || !DelayedBoundary)
      continue;

    llvm::sort(Candidates,
               [](const MacroStateGapCarryCandidate &LHS,
                  const MacroStateGapCarryCandidate &RHS) {
                 if (LHS.Interval.begin != RHS.Interval.begin)
                   return LHS.Interval.begin < RHS.Interval.begin;
                 return LHS.Directive->id < RHS.Directive->id;
               });

    const uint64_t NewStart = Candidates.front().Interval.begin;
    if (SourceRangeOverlapsFinalTUEditExcept(NewStart, Edit.start, EditIndex))
      continue;

    bool Admissible = true;
    for (const MacroStateGapCarryCandidate &Candidate : Candidates) {
      uint64_t Cursor = Candidate.Interval.end;
      for (const MacroStateGapCarryCandidate &Other : Candidates) {
        if (Other.Interval.begin <= Cursor)
          continue;
        if (Other.Interval.begin > Edit.start)
          break;
        StringRef Chunk = tuBytes_.slice(Cursor, Other.Interval.begin);
        StringRef Following = tuBytes_.slice(Other.Interval.begin, Edit.start);
        if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
                *Candidate.Directive, Candidate.Name, Chunk, Following)) {
          Admissible = false;
          break;
        }
        Cursor = Other.Interval.end;
      }
      if (!Admissible)
        break;
      if (Cursor < Edit.start) {
        StringRef Chunk = tuBytes_.slice(Cursor, Edit.start);
        if (MacroStateProof().SourceChunkObservesMacroStateDirectiveWhenCrossed(
                *Candidate.Directive, Candidate.Name, Chunk,
                StringRef(Edit.text))) {
          Admissible = false;
          break;
        }
      }
    }
    if (!Admissible)
      continue;
    if (*DelayedBoundary < Edit.end || *DelayedBoundary > tuBytes_.size())
      continue;

    std::string Replacement;
    Replacement.reserve((Edit.start - NewStart) + Edit.text.size() +
                        (*DelayedBoundary - Edit.end) + 64);
    uint64_t Cursor = NewStart;
    for (const MacroStateGapCarryCandidate &Candidate : Candidates) {
      if (Cursor > Candidate.Interval.begin) {
        Admissible = false;
        break;
      }
      Replacement.append(tuBytes_.begin() + Cursor,
                         tuBytes_.begin() + Candidate.Interval.begin);
      Cursor = Candidate.Interval.end;
    }
    if (!Admissible)
      continue;

    Replacement.append(tuBytes_.begin() + Cursor, tuBytes_.begin() + Edit.start);
    Replacement.append(Edit.text);
    Replacement.append(tuBytes_.begin() + Edit.end,
                       tuBytes_.begin() + *DelayedBoundary);
    if (!Replacement.empty() && Replacement.back() == '\\')
      continue;
    if (!Replacement.empty() && Replacement.back() != '\n')
      Replacement.push_back('\n');
    for (const MacroStateGapCarryCandidate &Candidate : Candidates)
      Replacement += Candidate.PreservationText;

    const uint64_t OldStart = Edit.start;
    const uint64_t OldEnd = Edit.end;
    RestageConservativeTUEdit(Edit, NewStart, *DelayedBoundary, Replacement);

    for (const MacroStateGapCarryCandidate &Candidate : Candidates) {
      (void)CheckMacroStateRepaired(
          *Candidate.Directive, StateMutationKind::MovedLater,
          "macro-gap-definition-carry",
          llvm::formatv("carried observed gap #define directive #{0} for "
                        "macro '{1}' after TU replacement [{2},{3})",
                        Candidate.Directive->id, Candidate.Name, OldStart,
                        OldEnd)
              .str());

      CarriedDirectiveIds.insert(Candidate.Directive->id);
      ++CarriedCount;
      REFOLD_LOG_WARN("macro/liveness",
                      "carrying observed gap #define after TU replacement: "
                      "macro='{0}' defDirective=#{1} edit=[{2},{3}) "
                      "widened=[{4},{5})",
                      Candidate.Name, Candidate.Directive->id, OldStart,
                      OldEnd, Edit.start, Edit.end);
    }
  }

  if (CarriedCount != 0)
    REFOLD_LOG_INFO("macro/liveness",
                    "carried {0} preserved #define directive(s) after "
                    "replacement payloads to keep edited tokens in B macro state",
                    CarriedCount);
}

bool MacroStateRepairContext::DefinitionDirectiveTouchedByTUEdit(
    const RefoldModel::MacroDirective &Directive) const {
  if (Directive.subkind != "#define")
    return false;
  return MacroDirectiveTouchedByTUEdit(Directive);
}

bool MacroStateRepairContext::PhysicalCallsiteAlreadyHasPatch(
    const RefoldModel::MacroInvocation &Invocation) const {
  if (!Invocation.invB || !Invocation.invE)
    return true;
  return dispatcher_.HasMacroPatchForInvocation(Invocation);
}

const RefoldModel::IncludeItem *
MacroStateRepairContext::OwningIncludeSiteForInvocationInTU(
    const RefoldModel::MacroInvocation &Invocation) const {
  return OutermostOwningIncludeSiteInTU(Invocation.ownerIncludeId);
}

bool MacroStateRepairContext::IncludeOwnedInvocationSurvivesTUEdits(
    const RefoldModel::MacroInvocation &Invocation) const {
  if (Invocation.invFile && PathIdentity().PathsEqual(*Invocation.invFile, tuPath_))
    return false;
  const RefoldModel::IncludeItem *Inc =
      OwningIncludeSiteForInvocationInTU(Invocation);
  if (!Inc)
    return false;
  return !IntervalOverlapsFinalTUEdit(Inc->siteB, Inc->siteE);
}

bool MacroStateRepairContext::InvocationCallsiteSurvivesTUEdits(
    const RefoldModel::MacroInvocation &Invocation) const {
  if (!Invocation.invFile || !Invocation.invB || !Invocation.invE ||
      *Invocation.invE < *Invocation.invB)
    return false;
  if (!PathIdentity().PathsEqual(*Invocation.invFile, tuPath_))
    return false;
  return !IntervalOverlapsFinalTUEdit(*Invocation.invB, *Invocation.invE);
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::FallbackActiveDefinitionForInvocation(
    const RefoldModel::MacroInvocation &Invocation) const {
  const RefoldModel::MacroDirective *Best = nullptr;
  for (const NamedMacroDirectiveRef &Ref : plan_.NamedMacroDirectives) {
    const RefoldModel::MacroDirective &Directive = *Ref.Directive;
    if (Directive.id >= Invocation.id)
      continue;
    if (StringRef(Ref.Name) != Invocation.name)
      continue;
    if (Best && Best->id > Directive.id)
      continue;
    Best = &Directive;
  }
  if (!Best || Best->subkind != "#define")
    return nullptr;
  return Best;
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::ActiveDefinitionForInvocation(
    const RefoldModel::MacroInvocation &Invocation) const {
  if (Invocation.definitionDirectiveId) {
    auto It = plan_.MacroDirectiveById.find(*Invocation.definitionDirectiveId);
    if (It != plan_.MacroDirectiveById.end() && It->second.Directive &&
        It->second.Directive->subkind == "#define")
      return It->second.Directive;
    return nullptr;
  }
  return FallbackActiveDefinitionForInvocation(Invocation);
}

bool MacroStateRepairContext::DefinitionHasOtherSurvivingSameNameTransition(
    const RefoldModel::MacroDirective &Definition, StringRef MacroName) const {
  for (const NamedMacroDirectiveRef &Ref : plan_.NamedMacroDirectives) {
    const RefoldModel::MacroDirective &Directive = *Ref.Directive;
    if (Directive.id == Definition.id)
      continue;
    if (StringRef(Ref.Name) != MacroName)
      continue;
    if (Directive.subkind != "#define" && Directive.subkind != "#undef")
      continue;

    // A transition consumed by a final TU edit does not remain in the emitted
    // macro-state stream.  It has its own liveness proof obligation, so it should
    // not make this definition look like part of a surviving same-name state
    // chain.
    if (MacroDirectiveTouchedByTUEdit(Directive))
      continue;

    return true;
  }

  return false;
}

void MacroStateRepairContext::RepairSurvivingDefinitionCallsites() {
  size_t PreservedDefinitionLivenessDirectives = 0;
  size_t ForcedDefinitionLivenessPatches = 0;

  for (const RefoldModel::MacroInvocation &Invocation :
       Model().GetMacroInvocations()) {
    if (Invocation.callerMacroId)
      continue;
    if (MacroTopology().IsInvocationInsideDefineDirective(Invocation))
      continue;

    const bool SurvivesAsTUCallsite =
        InvocationCallsiteSurvivesTUEdits(Invocation);
    const bool SurvivesAsIncludeCallsite =
        IncludeOwnedInvocationSurvivesTUEdits(Invocation);
    if (!SurvivesAsTUCallsite && !SurvivesAsIncludeCallsite)
      continue;

    const RefoldModel::MacroDirective *Definition =
        ActiveDefinitionForInvocation(Invocation);
    if (!Definition || !DefinitionDirectiveTouchedByTUEdit(*Definition))
      continue;

    if (PhysicalCallsiteAlreadyHasPatch(Invocation))
      continue;

    bool PreservedDefinition = false;
    const bool HasOtherSurvivingSameNameTransition =
        DefinitionHasOtherSurvivingSameNameTransition(*Definition,
                                                      Invocation.name);
    const bool MayPreserveDefinition =
        SurvivesAsIncludeCallsite ||
        (SurvivesAsTUCallsite && !HasOtherSurvivingSameNameTransition);
    if (MayPreserveDefinition) {
      if (std::optional<size_t> EditIndex =
              FinalTUEditContainingMacroDirective(*Definition)) {
        std::optional<MacroStatePreservationPlacement> Placement;
        if (!plan_.PreservedDefinitionDirectiveIds.contains(Definition->id))
          Placement = TryQueueMacroStateDirectivePreservation(
              *EditIndex, *Definition, Invocation.name,
              /*ObservedDefinition=*/nullptr);
        else
          Placement = MacroStatePreservationPlacement::BeforeReplacement;

        if (Placement) {
          PreservedDefinition = true;
          if (!plan_.PreservedDefinitionDirectiveIds.contains(Definition->id)) {
            plan_.PreservedDefinitionDirectiveIds.insert(Definition->id);
            ++PreservedDefinitionLivenessDirectives;
            REFOLD_LOG_WARN(
                "macro/liveness",
                "preserving consumed #define for surviving macro callsite: "
                "macro='{0}' defDirective=#{1} inv=#{2} edit=[{3},{4}) "
                "placement={5}",
                Invocation.name, Definition->id, Invocation.id,
                tuEdits_[*EditIndex].start, tuEdits_[*EditIndex].end,
                MacroStatePreservationPlacementName(*Placement));
          }
        }
      }
    }

    if (PreservedDefinition) {
      (void)CheckMacroStateRepaired(
          *Definition, StateMutationKind::Replayed,
          "macro-definition-liveness",
          llvm::formatv("preserved consumed definition #{0} for surviving "
                        "invocation #{1} of macro '{2}'",
                        Definition->id, Invocation.id, Invocation.name)
              .str());
      continue;
    }

    if (SurvivesAsIncludeCallsite) {
      (void)CheckMacroStateTerminal(
          *Definition, StateMutationKind::Consumed,
          "macro-definition-liveness",
          llvm::formatv("macro '{0}' invocation #{1} survives inside preserved "
                        "include site, but active definition directive #{2} "
                        "was consumed by a TU edit and the directive could not "
                        "be preserved in any proved placement before the "
                        "surviving include observes macro state",
                        Invocation.name, Invocation.id, Definition->id)
              .str(),
          /*RequireKnownObserver=*/true);
      continue;
    }

    // If the #define cannot be safely preserved, the only structural repair is
    // to remove this call site's dependency on that macro state by emitting the
    // B-side expansion at the call site.
    std::optional<WholeCoverPlan> WholePlan =
        MacroPatchPlanner().ComputeWholeCoverPlan(Invocation);
    if (!WholePlan) {
      (void)CheckMacroStateTerminal(
          *Definition, StateMutationKind::Consumed,
          "macro-definition-liveness",
          llvm::formatv("macro '{0}' invocation #{1} survives but active "
                        "definition directive #{2} was consumed by a TU edit; "
                        "the #define could not be preserved in any proved "
                        "placement and no whole-cover realization is available",
                        Invocation.name, Invocation.id, Definition->id)
              .str(),
          /*RequireKnownObserver=*/true);
      continue;
    }

    (void)CheckMacroStateMaterialized(
        *Definition, StateMutationKind::Materialized,
        "macro-definition-liveness",
        llvm::formatv("materialized surviving invocation #{0} of macro '{1}' "
                      "after consuming active definition #{2}",
                      Invocation.id, Invocation.name, Definition->id)
            .str());

    MacroPatch Patch{*Invocation.invB, *Invocation.invE,
                     WholePlan->clippedText, Invocation.id};
    ProofLattice().StampMacroWholeCoverRealizationPatch(Patch, *WholePlan,
                                                        Invocation);
    MacroPatchPlanner().StampMacroPatchOwnerWitness(
        Patch, Invocation.ownerIncludeId
                   ? Owner::Include(*Invocation.ownerIncludeId)
                   : Owner::TU());

    // Reuse an existing physical-callsite key if one was already allocated for
    // the same byte interval. Otherwise use this invocation id as the stable
    // key for the forced whole-cover patch.
    RefoldStructuralHunkDispatcher::MacroPatchStagingSlot StagingSlot =
        dispatcher_.PrepareMacroPatchStagingSlot(Invocation);
    dispatcher_.StageMacroPatch(StagingSlot, std::move(Patch));
    ++ForcedDefinitionLivenessPatches;

    REFOLD_LOG_WARN("macro/liveness",
                    "forced whole-cover macro realization because active "
                    "definition was consumed by TU edit and could not be "
                    "preserved: macro='{0}' defDirective=#{1} inv=#{2} "
                    "invBytes=[{3},{4})",
                    Invocation.name, Definition->id, Invocation.id,
                    *Invocation.invB, *Invocation.invE);
  }

  if (TerminalSink().HasRequest())
    return;

  if (PreservedDefinitionLivenessDirectives != 0) {
    REFOLD_LOG_INFO("macro/liveness",
                    "preserved {0} consumed #define directive(s) needed by "
                    "surviving macro callsite(s)",
                    PreservedDefinitionLivenessDirectives);
  }

  if (ForcedDefinitionLivenessPatches != 0) {
    REFOLD_LOG_INFO("macro/liveness",
                    "forced {0} macro callsite(s) to whole-cover realization "
                    "after definition-removing TU edits",
                    ForcedDefinitionLivenessPatches);
  }
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::PreviousLiveDefinitionBeforeDirective(
    const NamedMacroDirectiveRef &UndefRef) const {
  const RefoldModel::MacroDirective *Active = nullptr;
  for (const NamedMacroDirectiveRef &Ref : plan_.NamedMacroDirectives) {
    const RefoldModel::MacroDirective &Directive = *Ref.Directive;
    if (Directive.id >= UndefRef.Directive->id)
      break;
    if (StringRef(Ref.Name) != StringRef(UndefRef.Name))
      continue;
    if (Directive.subkind == "#define")
      Active = &Directive;
    else if (Directive.subkind == "#undef")
      Active = nullptr;
  }
  return Active;
}

bool MacroStateRepairContext::DirectiveSurvivesTUEdits(
    const RefoldModel::MacroDirective &Directive) const {
  return !MacroDirectiveTouchedByTUEdit(Directive);
}

size_t MacroStateRepairContext::PreserveConsumedUndefs() {
  size_t UndefLivenessHazards = 0;
  for (const NamedMacroDirectiveRef &Ref : plan_.NamedMacroDirectives) {
    const RefoldModel::MacroDirective &UndefDirective = *Ref.Directive;
    if (UndefDirective.subkind != "#undef")
      continue;

    std::optional<size_t> EditIndex =
        FinalTUEditContainingMacroDirective(UndefDirective);
    if (!EditIndex)
      continue;

    const RefoldModel::MacroDirective *PreviousDefinition =
        PreviousLiveDefinitionBeforeDirective(Ref);
    if (!PreviousDefinition || !DirectiveSurvivesTUEdits(*PreviousDefinition))
      continue;

    std::optional<MacroStatePreservationPlacement> Placement =
        TryQueueMacroStateDirectivePreservation(*EditIndex, UndefDirective,
                                                Ref.Name, PreviousDefinition);
    if (!Placement) {
      (void)CheckMacroStateTerminal(
          UndefDirective, StateMutationKind::Consumed, "macro-undef-liveness",
          llvm::formatv("#undef directive #{0} for macro '{1}' was consumed "
                        "by TU edit [{2},{3}), but it could not be preserved "
                        "without exposing the replacement payload to the macro "
                        "name or breaking the replacement/suffix boundary",
                        UndefDirective.id, Ref.Name, tuEdits_[*EditIndex].start,
                        tuEdits_[*EditIndex].end)
              .str(),
          /*RequireKnownObserver=*/true);
      continue;
    }

    (void)CheckMacroStateRepaired(
        UndefDirective, MutationForMacroStatePreservationPlacement(*Placement),
        "macro-undef-liveness",
        llvm::formatv("preserved consumed #undef directive #{0} for macro '{1}' "
                      "using {2} placement",
                      UndefDirective.id, Ref.Name,
                      MacroStatePreservationPlacementName(*Placement))
            .str());

    ++UndefLivenessHazards;
    REFOLD_LOG_WARN("macro/liveness",
                    "preserving consumed #undef to prevent resurrected macro "
                    "definition: macro='{0}' undefDirective=#{1} priorDefine=#{2} "
                    "edit=[{3},{4}) placement={5}",
                    Ref.Name, UndefDirective.id, PreviousDefinition->id,
                    tuEdits_[*EditIndex].start, tuEdits_[*EditIndex].end,
                    MacroStatePreservationPlacementName(*Placement));
  }

  return UndefLivenessHazards;
}

void MacroStateRepairContext::ApplyQueuedMacroStatePreservations() {
  for (auto &Entry : macroStatePreservationsByEdit_) {
    TextEdit &Edit = tuEdits_[Entry.first];
    SmallVector<MacroStatePreservation, 8> Preservations(Entry.second.begin(),
                                                         Entry.second.end());
    llvm::sort(Preservations,
               [](const MacroStatePreservation &LHS,
                  const MacroStatePreservation &RHS) {
                 if (LHS.Placement != RHS.Placement)
                   return LHS.Placement < RHS.Placement;
                 if (LHS.Directive->siteB != RHS.Directive->siteB)
                   return LHS.Directive->siteB < RHS.Directive->siteB;
                 return LHS.Directive->id < RHS.Directive->id;
               });

    std::string Prefix;
    std::string Suffix;
    std::map<size_t, std::string> InteriorInsertions;
    for (const MacroStatePreservation &Preservation : Preservations) {
      if (!Preservation.Directive)
        continue;
      if (Preservation.Placement ==
          MacroStatePreservationPlacement::BeforeReplacement) {
        Prefix += DirectiveTextForPreservation(*Preservation.Directive);
        continue;
      }

      if (Preservation.Placement ==
          MacroStatePreservationPlacement::AdvancedBeforeReplacement) {
        // Advanced-before-replacement repairs widen and rewrite the owning edit
        // immediately because they move the directive to a source byte boundary
        // outside the original edit start, not to a replacement-local offset.
        continue;
      }

      if (Preservation.Placement ==
          MacroStatePreservationPlacement::InsideReplacement) {
        InteriorInsertions[Preservation.ReplacementOffset] +=
            DirectiveTextForPreservation(*Preservation.Directive);
        continue;
      }

      if (Suffix.empty() && !Edit.text.empty() && Edit.text.back() != '\n')
        Suffix.push_back('\n');
      Suffix += DirectiveTextForPreservation(*Preservation.Directive);
    }

    if (!InteriorInsertions.empty()) {
      std::string ReplacementWithInteriorPreservations;
      size_t Cursor = 0;
      for (const auto &Insertion : InteriorInsertions) {
        const size_t Offset = std::min(Insertion.first, Edit.text.size());
        ReplacementWithInteriorPreservations.append(Edit.text.begin() + Cursor,
                                                    Edit.text.begin() + Offset);
        ReplacementWithInteriorPreservations += Insertion.second;
        Cursor = Offset;
      }
      ReplacementWithInteriorPreservations.append(Edit.text.begin() + Cursor,
                                                  Edit.text.end());
      Edit.text = std::move(ReplacementWithInteriorPreservations);
    }
    if (!Prefix.empty())
      Edit.text.insert(0, Prefix);
    if (!Suffix.empty())
      Edit.text.append(Suffix);
    AttachConservativeTUCarrier(Edit);
  }
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

  const size_t UndefLivenessHazards = PreserveConsumedUndefs();
  if (TerminalSink().HasRequest()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted after macro-undef liveness "
                     "repair; terminal fallback will be emitted");
    return false;
  }

  ApplyQueuedMacroStatePreservations();

  if (UndefLivenessHazards != 0) {
    REFOLD_LOG_INFO("macro/liveness",
                    "preserved {0} consumed #undef directive(s) to keep suffix "
                    "macro state equivalent after source replacement",
                    UndefLivenessHazards);
  }

  return true;
}

bool MacroStateRepairContext::MaterializedIncludeSubtreeOwnsMacroDirective(
    const RefoldModel::IncludeItem &Root,
    const RefoldModel::MacroDirective &Directive) const {
  if (!Directive.ownerIncludeId)
    return false;
  const RefoldModel::IncludeItem *Cur =
      Model().GetIncludeById(*Directive.ownerIncludeId);
  while (Cur) {
    if (Cur->id == Root.id)
      return true;
    if (!Cur->parent)
      return false;
    Cur = Model().GetIncludeById(*Cur->parent);
  }
  return false;
}

bool MacroStateRepairContext::MaterializedReplacementPreservesDirectiveAncestry(
    const RefoldModel::IncludeItem &Root,
    const RefoldModel::MacroDirective &Directive,
    const TextEdit &ReplacementEdit) const {
  if (!Directive.ownerIncludeId)
    return false;
  const RefoldModel::IncludeItem *Cur =
      Model().GetIncludeById(*Directive.ownerIncludeId);
  while (Cur) {
    if (IncludeDirectiveAppearsAtLineStart(ReplacementEdit, *Cur))
      return true;
    if (Cur->id == Root.id)
      break;
    if (!Cur->parent)
      break;
    Cur = Model().GetIncludeById(*Cur->parent);
  }
  return false;
}

bool MacroStateRepairContext::InvocationSurvivesAfterMaterializedInclude(
    const RefoldModel::MacroInvocation &Invocation,
    const RefoldModel::IncludeItem &MaterializedInclude,
    uint64_t MaterializedSiteEnd) const {
  if (InvocationCallsiteSurvivesTUEdits(Invocation))
    return Invocation.invB && *Invocation.invB >= MaterializedSiteEnd;

  if (!IncludeOwnedInvocationSurvivesTUEdits(Invocation))
    return false;
  const RefoldModel::IncludeItem *Owner =
      OwningIncludeSiteForInvocationInTU(Invocation);
  return Owner && Owner->siteB >= MaterializedSiteEnd &&
         Owner->id != MaterializedInclude.id;
}

bool MacroStateRepairContext::MaterializedIncludeNeedsDefinitionAfterward(
    const RefoldModel::IncludeItem &MaterializedInclude,
    uint64_t MaterializedSiteEnd,
    const RefoldModel::MacroDirective &Definition) const {
  for (const RefoldModel::MacroInvocation &Invocation :
       Model().GetMacroInvocations()) {
    if (Invocation.callerMacroId ||
        MacroTopology().IsInvocationInsideDefineDirective(Invocation))
      continue;
    if (PhysicalCallsiteAlreadyHasPatch(Invocation))
      continue;
    const RefoldModel::MacroDirective *Active =
        ActiveDefinitionForInvocation(Invocation);
    if (!Active || Active->id != Definition.id)
      continue;
    if (InvocationSurvivesAfterMaterializedInclude(
            Invocation, MaterializedInclude, MaterializedSiteEnd))
      return true;
  }
  return false;
}

bool MacroStateRepairContext::RepairConsumedDefinitionsForMaterializedInclude(
    const RefoldModel::IncludeItem &MaterializedInclude,
    uint64_t MaterializedSiteBegin, uint64_t MaterializedSiteEnd,
    std::string &ReplacementText) {
  TextEdit ReplacementProbe{MaterializedSiteBegin, MaterializedSiteEnd,
                            ReplacementText, std::nullopt, std::nullopt,
                            {}, {}, {}};
  std::string PreservedDirectivePrefix;

  for (const NamedMacroDirectiveRef &Ref : plan_.NamedMacroDirectives) {
    const RefoldModel::MacroDirective &Definition = *Ref.Directive;
    if (Definition.subkind != "#define")
      continue;
    if (plan_.PreservedDefinitionDirectiveIds.contains(Definition.id))
      continue;
    if (!MaterializedIncludeSubtreeOwnsMacroDirective(MaterializedInclude,
                                                      Definition))
      continue;
    if (MaterializedReplacementPreservesDirectiveAncestry(
            MaterializedInclude, Definition, ReplacementProbe))
      continue;
    if (MacroStateDirectiveAppearsAtLineStart(ReplacementProbe, Definition))
      continue;
    if (!MaterializedIncludeNeedsDefinitionAfterward(
            MaterializedInclude, MaterializedSiteEnd, Definition))
      continue;

    if (DefinitionHasOtherSurvivingSameNameTransition(Definition, Ref.Name)) {
      (void)CheckMacroStateTerminal(
          Definition, StateMutationKind::Consumed,
          "include/materialized-macro-state",
          llvm::formatv("materialized include inc#{0} consumes definition #{1} "
                        "for macro '{2}', but another same-name transition "
                        "survives in the suffix",
                        MaterializedInclude.id, Definition.id, Ref.Name)
              .str(),
          /*RequireKnownObserver=*/true);
      return false;
    }

    if (FirstReplacementObservationOffset(ReplacementProbe, Definition,
                                          Ref.Name)) {
      (void)CheckMacroStateTerminal(
          Definition, StateMutationKind::Consumed,
          "include/materialized-macro-state",
          llvm::formatv("materialized include inc#{0} consumes definition #{1} "
                        "for macro '{2}', but the materialized payload itself "
                        "observes that macro",
                        MaterializedInclude.id, Definition.id, Ref.Name)
              .str(),
          /*RequireKnownObserver=*/true);
      return false;
    }

    std::string DirectiveText = DirectiveTextForPreservation(Definition);
    if (DirectiveText.empty())
      continue;
    if (!DirectiveText.empty() && DirectiveText.back() != '\n')
      DirectiveText.push_back('\n');

    (void)CheckMacroStateRepaired(
        Definition, StateMutationKind::Replayed,
        "include/materialized-macro-state",
        llvm::formatv("preserved include-owned definition #{0} before "
                      "materialized include inc#{1} for surviving suffix macro "
                      "'{2}'",
                      Definition.id, MaterializedInclude.id, Ref.Name)
            .str());

    PreservedDirectivePrefix += DirectiveText;
    plan_.PreservedDefinitionDirectiveIds.insert(Definition.id);
  }

  if (!PreservedDirectivePrefix.empty())
    ReplacementText.insert(0, PreservedDirectivePrefix);

  return true;
}

} // namespace

RefoldMacroStateRepairPlanner::RefoldMacroStateRepairPlanner(Dependencies Deps)
    : deps_(Deps) {}

RefoldMacroStateRepairPlanner::MacroStateRepairPlan
RefoldMacroStateRepairPlanner::Plan(
    const MacroStateRepairRequest &Request) const {
  MacroStateRepairPlan RepairPlan;
  MacroStateRepairContext Context(deps_, Request, RepairPlan);
  RepairPlan.Success = Context.RunInitialRepair();
  return RepairPlan;
}

void RefoldMacroStateRepairPlanner::CarryObservedGapDefinitionsAfterReplacements(
    MacroStateRepairPlan &Plan, const MacroStateRepairRequest &Request) const {
  MacroStateRepairContext Context(deps_, Request, Plan);
  Context.CarryObservedGapDefinitionsAfterReplacements();
}

bool RefoldMacroStateRepairPlanner::RepairConsumedDefinitionsForMaterializedInclude(
    MacroStateRepairPlan &Plan, const MacroStateRepairRequest &Request,
    const RefoldModel::IncludeItem &MaterializedInclude,
    uint64_t MaterializedSiteBegin, uint64_t MaterializedSiteEnd,
    std::string &ReplacementText) const {
  MacroStateRepairContext Context(deps_, Request, Plan);
  return Context.RepairConsumedDefinitionsForMaterializedInclude(
      MaterializedInclude, MaterializedSiteBegin, MaterializedSiteEnd,
      ReplacementText);
}

} // namespace refold
} // namespace clang

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
  const RefoldModel::MacroDirective *directive = nullptr;
  MacroStatePreservationPlacement placement =
      MacroStatePreservationPlacement::BeforeReplacement;
  size_t replacementOffset = 0;
};

struct MacroStateSourceTransition {
  MacroDirectiveSourceInterval interval;
  std::string text;
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
        tuBytes_(request.tuBytes) {
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

  /// Reconstructs the macro-state transition represented by a source directive.
  std::optional<MacroStateSourceTransition> MacroStateSourceTransitionFor(
      const RefoldModel::MacroDirective &directive) const;
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
  /// Validates proof evidence for a materialized macro-state transition.
  StateTransitionProof
  CheckMacroStateMaterialized(const RefoldModel::MacroDirective &directive,
                              StateMutationKind mutation, StringRef stage,
                              StringRef detail,
                              bool requireKnownObserver = false) const;
  /// Validates terminal fallback evidence for a macro-state transition.
  StateTransitionProof
  CheckMacroStateTerminal(const RefoldModel::MacroDirective &directive,
                          StateMutationKind mutation, StringRef stage,
                          StringRef detail,
                          bool requireKnownObserver = true) const;
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

  /// Restages a conservative TU edit after a repair changes its byte range or
  /// replacement text.
  void RestageConservativeTUEdit(TextEdit &edit, uint64_t start, uint64_t end,
                                 StringRef replacement);
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
  /// Applies queued macro-state preservation edits in deterministic edit order.
  void ApplyQueuedMacroStatePreservations();

  /// Returns whether a definition directive is consumed or altered by TU edits.
  bool DefinitionDirectiveTouchedByTUEdit(
      const RefoldModel::MacroDirective &directive) const;
  /// Returns whether a physical macro invocation callsite already has a patch.
  bool PhysicalCallsiteAlreadyHasPatch(
      const RefoldModel::MacroInvocation &invocation) const;
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
  std::map<size_t, SmallVector<MacroStatePreservation, 4>>
      macroStatePreservationsByEdit_;
};

void MacroStateRepairContext::BuildDirectiveIndexOnce() {
  if (plan_.directiveIndexBuilt)
    return;

  plan_.macroDirectiveById.clear();
  plan_.namedMacroDirectives.clear();

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

bool MacroStateRepairContext::MacroStateDirectiveAppearsAtLineStart(
    const TextEdit &edit, const RefoldModel::MacroDirective &directive) const {
  return !directive.text.empty() &&
         stringutils::containsAtLineStartAfterIndent(edit.text, directive.text);
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
  std::string textLocal = directive.text.str();
  if (textLocal.empty() || textLocal.back() != '\n')
    textLocal.push_back('\n');
  return textLocal;
}

std::optional<MacroStateSourceTransition>
MacroStateRepairContext::MacroStateSourceTransitionFor(
    const RefoldModel::MacroDirective &directive) const {
  if (PathIdentity().PathsEqual(directive.sitePath, tuPath_) &&
      !directive.ownerIncludeId) {
    std::optional<MacroDirectiveSourceInterval> intervalLocal =
        MacroDirectiveFullSourceInterval(directive);
    if (!intervalLocal)
      return std::nullopt;
    return MacroStateSourceTransition{*intervalLocal,
                                      DirectiveTextForPreservation(directive)};
  }

  const RefoldModel::IncludeItem *inc = OwningIncludeSiteInTU(directive);
  if (!inc)
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
  return MacroStateSourceTransition{interval, std::move(spelling)};
}

const RefoldModel::MacroDirective *
MacroStateRepairContext::ActiveDefinitionAtSourceOffset(StringRef macroName,
                                                        uint64_t offset) const {
  const RefoldModel::MacroDirective *active = nullptr;
  uint64_t activeEnd = 0;
  for (const NamedMacroDirectiveRef &ref : plan_.namedMacroDirectives) {
    const RefoldModel::MacroDirective &candidate = *ref.directive;
    if (StringRef(ref.name) != macroName)
      continue;
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

StateTransitionProof MacroStateRepairContext::CheckMacroStateMaterialized(
    const RefoldModel::MacroDirective &directive, StateMutationKind mutation,
    StringRef stage, StringRef detail, bool requireKnownObserver) const {
  const OwnerStateBoundary boundary = MacroDirectiveSuffixBoundary(directive);
  return CheckMacroStateWithWitness(
      boundary, mutation,
      OwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::OwnerMaterialization,
          OwnerStateComponent::MacroState, boundary, detail),
      stage, detail, requireKnownObserver);
}

StateTransitionProof MacroStateRepairContext::CheckMacroStateTerminal(
    const RefoldModel::MacroDirective &directive, StateMutationKind mutation,
    StringRef stage, StringRef detail, bool requireKnownObserver) const {
  const OwnerStateBoundary boundary = MacroDirectiveSuffixBoundary(directive);
  return CheckMacroStateWithWitness(
      boundary, mutation,
      OwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::TerminalStateFailure,
          OwnerStateComponent::MacroState, boundary, detail),
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
                .BuildAcceptedTUTextEditCandidate(
                    AcceptedPathKind::TUByteSpanConservativeEdit, edit.start,
                    edit.end, StringRef(edit.text)));
}

void MacroStateRepairContext::RestageConservativeTUEdit(TextEdit &edit,
                                                        uint64_t start,
                                                        uint64_t end,
                                                        StringRef replacement) {
  ResyncOutcome resync = TextEditAssembler().ApplyResyncOrPend(
      tuBytes_, start, end, replacement, tuPath_);
  edit.start = start;
  edit.end = end;
  edit.text = std::move(resync.text);
  edit.pending = std::move(resync.pending);
  edit.lineControlPruneCandidates =
      std::move(resync.lineControlPruneCandidates);

  // A repair edit is no longer the original direct TU hunk edit. It now carries
  // macro-state transition bytes around the original replacement.
  edit.isDirectTUHunkEdit = false;
  edit.directTUHunkIndex.reset();
  edit.directTUHunkAStart.reset();
  edit.directTUHunkAEnd.reset();
  edit.directTUHunkBStart.reset();
  edit.directTUHunkBEnd.reset();
  edit.directTURawStart.reset();
  edit.directTURawEnd.reset();
  edit.directTUFinalStart = edit.start;
  edit.directTUFinalEnd = edit.end;
  AttachConservativeTUCarrier(edit);
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
      RestageConservativeTUEdit(edit, lineStart, undefTransition->interval.end,
                                replacement);

      (void)CheckMacroStateRepaired(
          undefDirective, StateMutationKind::MovedEarlier,
          "macro-undef-liveness",
          llvm::formatv("advanced preserved #undef directive #{0} for macro "
                        "'{1}' before observing replacement [{2},{3})",
                        undefDirective.id, undefRef.name, oldStart, oldEnd)
              .str());

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

  const uint64_t oldStart = edit.start;
  RestageConservativeTUEdit(edit, lineStart, edit.end, replacement);

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
    for (const MacroStateGapCarryCandidate &candidate : candidates)
      replacement += candidate.preservationText;

    const uint64_t oldStart = edit.start;
    const uint64_t oldEnd = edit.end;
    RestageConservativeTUEdit(edit, newStart, *delayedBoundary, replacement);

    for (const MacroStateGapCarryCandidate &candidate : candidates) {
      (void)CheckMacroStateRepaired(
          *candidate.directive, StateMutationKind::MovedLater,
          "macro-gap-definition-carry",
          llvm::formatv("carried observed gap #define directive #{0} for "
                        "macro '{1}' after TU replacement [{2},{3})",
                        candidate.directive->id, candidate.name, oldStart,
                        oldEnd)
              .str());

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

bool MacroStateRepairContext::PhysicalCallsiteAlreadyHasPatch(
    const RefoldModel::MacroInvocation &invocation) const {
  if (!invocation.invB || !invocation.invE)
    return true;
  return dispatcher_.HasMacroPatchForInvocation(invocation);
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

    if (PhysicalCallsiteAlreadyHasPatch(invocation))
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
      (void)CheckMacroStateRepaired(
          *definition, StateMutationKind::Replayed, "macro-definition-liveness",
          llvm::formatv("preserved consumed definition #{0} for surviving "
                        "invocation #{1} of macro '{2}'",
                        definition->id, invocation.id, invocation.name)
              .str());
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
          /*RequireKnownObserver=*/true);
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
          /*RequireKnownObserver=*/true);
      continue;
    }

    (void)CheckMacroStateMaterialized(
        *definition, StateMutationKind::Materialized,
        "macro-definition-liveness",
        llvm::formatv("materialized surviving invocation #{0} of macro '{1}' "
                      "after consuming active definition #{2}",
                      invocation.id, invocation.name, definition->id)
            .str());

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

    (void)CheckMacroStateRepaired(
        undefDirective, MutationForMacroStatePreservationPlacement(*placement),
        "macro-undef-liveness",
        llvm::formatv(
            "preserved consumed #undef directive #{0} for macro '{1}' "
            "using {2} placement",
            undefDirective.id, ref.name,
            MacroStatePreservationPlacementName(*placement))
            .str());

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

void MacroStateRepairContext::ApplyQueuedMacroStatePreservations() {
  for (auto &entry : macroStatePreservationsByEdit_) {
    TextEdit &edit = tuEdits_[entry.first];
    SmallVector<MacroStatePreservation, 8> Preservations(entry.second.begin(),
                                                         entry.second.end());
    llvm::sort(Preservations, [](const MacroStatePreservation &lhs,
                                 const MacroStatePreservation &rhs) {
      if (lhs.placement != rhs.placement)
        return lhs.placement < rhs.placement;
      if (lhs.directive->siteB != rhs.directive->siteB)
        return lhs.directive->siteB < rhs.directive->siteB;
      return lhs.directive->id < rhs.directive->id;
    });

    std::string prefix;
    std::string suffix;
    std::map<size_t, std::string> interiorInsertions;
    for (const MacroStatePreservation &preservation : Preservations) {
      if (!preservation.directive)
        continue;
      if (preservation.placement ==
          MacroStatePreservationPlacement::BeforeReplacement) {
        prefix += DirectiveTextForPreservation(*preservation.directive);
        continue;
      }

      if (preservation.placement ==
          MacroStatePreservationPlacement::AdvancedBeforeReplacement) {
        // Advanced-before-replacement repairs widen and rewrite the owning edit
        // immediately because they move the directive to a source byte boundary
        // outside the original edit start, not to a replacement-local offset.
        continue;
      }

      if (preservation.placement ==
          MacroStatePreservationPlacement::InsideReplacement) {
        interiorInsertions[preservation.replacementOffset] +=
            DirectiveTextForPreservation(*preservation.directive);
        continue;
      }

      if (suffix.empty() && !edit.text.empty() && edit.text.back() != '\n')
        suffix.push_back('\n');
      suffix += DirectiveTextForPreservation(*preservation.directive);
    }

    if (!interiorInsertions.empty()) {
      std::string replacementWithInteriorPreservations;
      size_t cursor = 0;
      for (const auto &insertion : interiorInsertions) {
        const size_t offset = std::min(insertion.first, edit.text.size());
        replacementWithInteriorPreservations.append(edit.text.begin() + cursor,
                                                    edit.text.begin() + offset);
        replacementWithInteriorPreservations += insertion.second;
        cursor = offset;
      }
      replacementWithInteriorPreservations.append(edit.text.begin() + cursor,
                                                  edit.text.end());
      edit.text = std::move(replacementWithInteriorPreservations);
    }
    if (!prefix.empty())
      edit.text.insert(0, prefix);
    if (!suffix.empty())
      edit.text.append(suffix);
    AttachConservativeTUCarrier(edit);
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

  const size_t undefLivenessHazards = PreserveConsumedUndefs();
  if (TerminalSink().HasRequest()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted after macro-undef liveness "
                     "repair; terminal fallback will be emitted");
    return false;
  }

  ApplyQueuedMacroStatePreservations();

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
    if (PhysicalCallsiteAlreadyHasPatch(invocation))
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
    if (!MaterializedIncludeNeedsDefinitionAfterward(
            materializedInclude, materializedSiteEnd, definition))
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
          /*RequireKnownObserver=*/true);
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
          /*RequireKnownObserver=*/true);
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

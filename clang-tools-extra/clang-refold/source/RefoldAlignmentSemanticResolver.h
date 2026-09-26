//===--- RefoldAlignmentSemanticResolver.h -----------------------*- C++ -*-===//
//
// Exact semantic resolution for non-forced weighted-LCS alignment anchors.
//
// The core certifier exposes only match edges used by every optimal path. This
// service may restore non-forced anchors only after complete optimal-map
// enumeration and isolated full-planner proofs establish either one identical
// realization or one globally least exact source-transformation class.
// Historical boundary choices remain proposal-only and require exact
// counterfactual validation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTSEMANTICRESOLVER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTSEMANTICRESOLVER_H

#include "source/DiffAlgorithms.h"
#include "source/RefoldAlignmentSemanticEvidence.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// Internal theorem-bearing alignment surface used by isolated simulations.
struct AlignmentSelectionOverride {
  std::vector<int64_t> selectedMap;
  std::vector<diffutils::LcsAnchorProof> selectedAnchorProofs;
  /// Preserve the parent certification envelope so isolated simulations pass
  /// the same per-window authority checks as the production planner.
  diffutils::LcsObjective globalObjective;
  bool globalObjectiveIsExact = false;
  bool allWindowsCertified = false;
  llvm::SmallVector<diffutils::LcsCertifiedBoundary, 1> certifiedBoundaries;
  llvm::SmallVector<diffutils::LcsCertificationWindow, 1>
      certificationWindows;
  /// Half-open A ranges of the windows whose alignment the
  /// structure-respecting repair chose.  A structural tiling tie inside one is
  /// settled in favour of the partition keeping protected structure in place;
  /// see `RefoldStructuralHunkTilingPlanner`.  Empty everywhere else, which
  /// leaves every tie declined as before.
  std::vector<std::pair<uint64_t, uint64_t>> structurePreservingTieRanges;
};

/// Diagnostic classification of one isolated alignment simulation.
///
/// This classification is evidence-only.  In particular, ProofIncomplete does
/// not mean that the candidate was proved semantically invalid; it means that
/// the current proof vocabulary could not compare that candidate completely.
enum class AlignmentSemanticSimulationDisposition : uint8_t {
  Accepted,
  TerminalFallback,
  ProofIncomplete,
};

/// Exact component payloads used by alignment equivalence theorems.
///
/// These payloads begin after owner/structure planning. The semantic resolver
/// deliberately excludes the initial A/B hunk partition and compares only the
/// realized source/state topology and requested concrete outputs.
struct AlignmentSemanticEquivalenceComponents {
  std::string structuralTilingWitnesses;
  std::string stagedTopology;
  std::string materializedMappings;
  std::string finalTU;
  /// Exact final-line-control pruning inputs captured before executable
  /// validation. Equal unpruned source is sufficient only when the candidate
  /// directive set and source mappings supplied to the deterministic pruner
  /// are also identical.
  std::string finalLineControlPlan;
  std::string destructiveMutationFootprint;
  std::string insertionFrontiers;
  std::string expansionIdentities;
  std::string semanticPostconditions;
  std::string realizationPostconditions;
  AlignmentSemanticPreservationFootprint preservationFootprint;
};

/// Result of running one complete structural planning simulation.
struct AlignmentSemanticSimulationResult {
  bool accepted = false;
  AlignmentSemanticSimulationDisposition disposition =
      AlignmentSemanticSimulationDisposition::ProofIncomplete;
  /// Byte-exact realized-source key after hunk/owner planning.  This excludes
  /// only the raw normalized A/B hunk partition; all staged carriers, physical
  /// mappings, state witnesses, source-graph outputs, and final source bytes
  /// remain part of the theorem.
  std::string realizationEquivalenceKey;
  /// Exact emitted-artifact theorem key. This deliberately excludes internal
  /// proof-carrier provenance after every candidate has independently passed
  /// the complete theorem audit; it retains the unpruned TU, materialized
  /// mappings, source-graph outputs, and final-line-control pruning inputs.
  std::string concreteOutputEquivalenceKey;
  std::string rejectionReason;
  AlignmentSemanticEquivalenceComponents components;
  /// For a `TerminalFallback` result, the half-open A token envelope each
  /// terminal request names, in request order; `std::nullopt` stands for a
  /// request that names none.  Read from the requests' structured failure
  /// context, never from their diagnostic text.
  std::vector<std::optional<std::pair<uint64_t, uint64_t>>>
      terminalRequestATokenRanges;
};

/// Exact reason one proposal anchor became mandatory.
enum class AlignmentSemanticAnchorBasis : uint8_t {
  /// Removing the anchor changes the independently proved final source.
  FinalSourceNecessary,
  /// Removing the anchor changes the complete semantic state postcondition.
  SemanticPostconditionNecessary,
  /// Removing the anchor yields a strictly more destructive source carrier.
  SourcePreservationNecessary,
  /// The anchor belongs to the deterministic representative selected after
  /// exact equivalence or source-mutation containment reduced every surviving
  /// complete map to one realized-source class.
  EquivalentRealizationRepresentative,
};

/// Durable evidence for one non-forced production anchor.
struct AlignmentSemanticAnchorEvidence {
  uint64_t aToken = 0;
  uint64_t bToken = 0;
  AlignmentSemanticAnchorBasis basis =
      AlignmentSemanticAnchorBasis::EquivalentRealizationRepresentative;
};

/// Durable proof that one ambiguity class has one structural realization.
struct AlignmentSemanticResolutionWitness {
  uint64_t witnessId = 0;
  uint64_t enumeratedMapCount = 0;
  uint64_t acceptedMapCount = 0;
  uint64_t rejectedMapCount = 0;
  bool completeEnumeration = false;
  std::string equivalenceKey;
  std::vector<int64_t> representativeMap;
  std::vector<AlignmentSemanticAnchorEvidence> anchorEvidence;
};

/// Source-layout queries read by the structure-respecting repair's preference
/// keys.
///
/// They describe where A tokens sit in the translation unit's own source, and
/// they only order candidates that are each realized and audited in full; no
/// answer here can admit a realization.  A token or gap the translation unit
/// does not spell answers false, which only withholds a preference.
struct AlignmentSourceLayoutQueries {
  /// Whether the A token is the first token on its logical source line.
  std::function<bool(uint64_t)> aTokenBeginsSourceLine;
  /// Whether the A token is the last token on its logical source line.
  std::function<bool(uint64_t)> aTokenEndsSourceLine;
  /// The sorted A gaps `g`, with `aBegin < g < aEnd`, whose source bytes are
  /// ordinary trivia holding a comment the comment-seam splitter preserves.
  std::function<std::vector<uint64_t>(uint64_t aBegin, uint64_t aEnd)>
      collectCommentGaps;
};

/// Exact semantic resolver for ambiguity left by the core LCS theorem.
class RefoldAlignmentSemanticResolver {
public:
  using SimulationCallback = std::function<AlignmentSemanticSimulationResult(
      const AlignmentSelectionOverride &)>;

  struct Dependencies {
    llvm::ArrayRef<llvm::StringRef> aLexemes;
    llvm::ArrayRef<llvm::StringRef> bLexemes;
    const diffutils::CertifiedLcsResult &coreAlignment;
    /// Exact producer-backed provenance for every A-token gap.  This is used
    /// only to explain the structural identity of candidate hunk frontiers.
    llvm::ArrayRef<diffutils::LcsAGapProvenance> aGapProvenance;
    /// Edited-side surface facts used to reconstruct the historical boundary
    /// map strictly as a proposal. These ranks never grant anchor authority.
    llvm::ArrayRef<diffutils::LcsBGapProvenance> bGapProvenance;
    SimulationCallback simulate;
    /// Materialize one certification window's all-optimal pair facts, which
    /// the partitioned certifier deliberately released. Returns true when
    /// `coreAlignment.HasCompleteSemanticOracleForWindow(windowIndex)` holds
    /// afterwards. Retention is requested one window at a time so peak
    /// quadratic storage stays bounded by the largest single window rather
    /// than the complete grid.
    std::function<bool(size_t)> retainWindowOracle;
    /// Release the facts retained for one window once it has been resolved.
    /// Committed anchors survive; only the quadratic payload is dropped.
    std::function<void(size_t)> releaseWindowOracle;
    /// Collect the sorted, unique A-token frontiers that the translation
    /// unit's protected preprocessing structure projects to: directives, and
    /// the edges of each directly included file's cover.  This is the same
    /// producer-backed surface that schedules certification partitions.  It is
    /// called at most once, and only when a window reaches the
    /// structure-respecting repair rule.
    std::function<std::vector<uint64_t>()> collectProtectedABoundaries;
    /// Layout queries for the structure-respecting repair's later keys.
    AlignmentSourceLayoutQueries sourceLayout;
  };

  struct ResolutionResult {
    std::vector<int64_t> selectedMap;
    std::vector<diffutils::LcsAnchorProof> selectedAnchorProofs;
    std::vector<AlignmentSemanticResolutionWitness> witnesses;
    bool committedEquivalentClass = false;
    bool completeEnumeration = false;
    /// A ranges of the windows committed by the structure-respecting repair;
    /// see `AlignmentSelectionOverride::structurePreservingTieRanges`.
    std::vector<std::pair<uint64_t, uint64_t>> structurePreservingTieRanges;
  };

  explicit RefoldAlignmentSemanticResolver(Dependencies deps);

  /// Resolve non-forced core-optimal maps through exact source-preservation,
  /// realized-source equivalence, and counterfactual theorems.
  ///
  /// Resolution is per certification window. Every window that can carry
  /// ambiguity at all is resolved -- see `WindowCarriesAmbiguity()` -- and each
  /// is resolved independently, in source order, with every committed window
  /// contributing its anchors to the base map used by the next. A window
  /// without retained facts, or one whose ambiguity the theorems below cannot
  /// close, keeps exactly its core-forced anchors and does not prevent an
  /// independent window from committing. The single-window complete-stream case
  /// reduces to the historical whole-stream theorem.
  ///
  /// Nothing here is skipped to save work. The only windows passed over are
  /// those the core theorem already determined completely, where resolution has
  /// no ambiguity to resolve and running it would return the same anchors after
  /// paying for them.
  ///
  /// Enumeration budgets are proof budgets only: exceeding one denies the
  /// commit rules that quantify over the refused set and cannot authorize a
  /// partial class or ranked winner.  A window with no rule left keeps its
  /// forced-only map.
  ResolutionResult Resolve() const;

private:
  /// Outcome of resolving the ambiguity inside one certification window.
  struct WindowResolution {
    bool committed = false;
    /// Complete-stream map: `baseMap` with this window's choice substituted.
    std::vector<int64_t> selectedMap;
    std::vector<AlignmentSemanticAnchorEvidence> anchorEvidence;
    std::string equivalenceKey;
    /// Number of distinct complete optimal maps enumerated through the window.
    ///
    /// Recorded whether or not the window commits, because it is what separates
    /// the three ways a window can end: an enumeration that never completed
    /// (zero), a window the core theorem had already determined (one), and real
    /// ambiguity that no commit rule closed (more than one).  A verdict log
    /// that could not tell those apart would report a decline where nothing was
    /// declined.
    uint64_t enumeratedMapCount = 0;
    /// True when the window's complete ground set could not be enumerated and
    /// the legacy boundary proposal committed over the maps carrying its
    /// required anchors instead.  `enumeratedMapCount` then counts those maps.
    bool enumeratedOnlyRequiredAnchorCarriers = false;
    uint64_t acceptedMapCount = 0;
    uint64_t rejectedMapCount = 0;
    /// True when `SelectStructureRespectingRepair()` chose this commit, so
    /// its realization must keep the structure the repair preferred.
    bool structureRespectingRepair = false;
  };

  /// Resolve the ambiguity inside one certification window.
  ///
  /// Candidate maps differ from `baseMap` only inside the window's A range, so
  /// every simulation observes identical anchors everywhere else and the
  /// comparison isolates this window's choice. `windowIndex` must satisfy
  /// `HasCompleteSemanticOracleForWindow()`.
  ///
  /// Each commit rule decides over its complete ground set, never a prefix:
  /// uniqueness is a property of the whole set, so committing on a prefix
  /// would be the ranked selection this resolver exists to avoid.  The
  /// observational and least-source-mutation rules' set is every optimal map
  /// of the window; the legacy boundary proposal's is the optimal maps that
  /// carry its required anchors, which it enumerates directly when the
  /// window's complete enumeration exceeds its proof budget.
  ///
  /// A window every rule declines, enumerated or not, is then offered to
  /// `SelectStructureRespectingRepair()`.  That is the one ranked selection
  /// here: it prefers a source layout among realizations that are each
  /// audited in full, and it is reached only when declining is itself proved
  /// to request terminal fallback, so it never replaces an admissible output.
  ///
  /// \p laterWindowCarriesAmbiguity is whether any certification window after
  /// this one can carry ambiguity.  When none can, this window's verdict feeds
  /// no later resolution, so its outcome is observed only through the emitted
  /// output; that is what lets an over-budget observational-irrelevance rule
  /// be retired once committing and declining are proved to emit the same
  /// output.
  WindowResolution
  ResolveCertificationWindow(size_t windowIndex,
                             llvm::ArrayRef<int64_t> baseMap,
                             bool laterWindowCarriesAmbiguity) const;

  /// Realize \p candidateMap through one complete planning simulation, reusing
  /// \p slot when it already holds this map's result.
  ///
  /// A simulation is a function of its candidate map alone: it plans a fresh
  /// engine over the run's fixed A and B streams under that alignment and reads
  /// nothing a sibling simulation writes.  Which commit rule asks for a map
  /// first, and whether a rule denied earlier would also have asked, therefore
  /// change only how many whole-translation-unit refolds the window pays for --
  /// never what any rule decides.
  ///
  /// Realizing on demand is what lets a rule whose set is small be reached
  /// without first paying for the sets of the rules that were already denied.
  ///
  /// \p windowIndex, \p mapIndex and \p mapCount name the realization in the
  /// log and are read for nothing else.  A realization is the most expensive
  /// step this tool takes, so each one reports itself rather than appearing as
  /// an unexplained repeat of the whole planning pipeline.
  ///
  /// \p tieRanges are added to the candidate's structure-preserving tie
  /// ranges; see `Simulate()`.
  const AlignmentSemanticSimulationResult &RealizeCandidateMap(
      size_t windowIndex, size_t mapIndex, size_t mapCount,
      llvm::ArrayRef<int64_t> candidateMap,
      std::optional<AlignmentSemanticSimulationResult> &slot,
      llvm::ArrayRef<std::pair<uint64_t, uint64_t>> tieRanges = {}) const;

  /// Realize \p selection through the simulation callback, after adding the
  /// windows the structure-respecting repair has already committed.
  ///
  /// A committed window is realized in production with its ties settled in
  /// favour of preserved structure, so every later simulation realizes it the
  /// same way; otherwise a later window would be compared against a
  /// realization production never emits.
  AlignmentSemanticSimulationResult
  Simulate(AlignmentSelectionOverride selection) const;

  /// The map the structure-respecting repair commits, and the census that
  /// selected it.
  struct StructureRespectingRepair {
    std::string equivalenceKey;
    std::vector<int64_t> selectedMap;
    /// Candidates realized for the decision, and how many of them share the
    /// committed output.
    uint64_t candidateCount = 0;
    uint64_t classSize = 0;
  };

  /// Select the map a window commits when declining it is proved
  /// inadmissible, by a preference over where its hunks sit in the source.
  ///
  /// Every other commit rule may decline, and a declining window keeps
  /// \p baseMap.  That map is not one of the window's optimal maps: it matches
  /// only the forced anchors, so its hunk is the envelope of every candidate's
  /// hunk and straddles every construct any of them straddles.  When that
  /// realization requests terminal fallback, declining is known to reach the
  /// whole-translation-unit carrier, and any accepted candidate -- each a
  /// complete, independently audited refold -- is strictly better.  The
  /// preference below therefore orders candidates; it admits none.
  ///
  /// The window is split at its forced anchors, exactly as enumeration splits
  /// it, and each sub-rectangle's optimal maps are enumerated up to the repair's
  /// own bound.  A sub-rectangle's runs of unmatched A tokens are its hunks, and
  /// its maps are narrowed by four keys in order, each applied only where it
  /// expresses a preference:
  ///
  ///   1. the least set, under inclusion, of straddled protected boundaries;
  ///   2. every run begins and ends a source line;
  ///   3. the fewest runs;
  ///   4. the least set, under inclusion, of straddled preservable comments.
  ///
  /// A boundary or comment gap is straddled when it lies strictly inside a
  /// run.  A sub-rectangle whose keys keep every map expresses no preference and
  /// stays at \p baseMap; the others vary over the maps they keep, and every
  /// combination is realized.  The window commits when those realizations
  /// accept exactly one concrete output and \p baseMap requests terminal
  /// fallback.
  ///
  /// When the combinations exceed the realization budget, \p baseMap is
  /// realized first, and only the preferred sub-rectangles that one of its
  /// terminal requests' A envelopes meets vary; the rest stay at \p baseMap,
  /// as declining would leave them.  This narrows which candidates are tried
  /// and admits none: each is still realized whole and must be the one
  /// accepted output.  It applies only when every request names an A
  /// envelope; otherwise the decline's failure cannot be located.
  ///
  /// It declines -- so the window keeps \p baseMap -- when no sub-rectangle
  /// expresses a preference, when one cannot be enumerated within the bound,
  /// when the combinations exceed the realization budget even so, when one is
  /// proof-incomplete, when they accept zero or several outputs, or when
  /// \p baseMap does not request terminal fallback.  The last check is what
  /// confines the rule to windows whose decline is inadmissible.
  ///
  /// Every combination is realized with the window's structural tiling ties
  /// settled in favour of preserved structure, which is how production will
  /// realize the committed map.  The decline configuration is realized as
  /// production would realize the decline, without that setting.
  std::optional<StructureRespectingRepair>
  SelectStructureRespectingRepair(size_t windowIndex,
                                  llvm::ArrayRef<int64_t> baseMap) const;

  /// Return the translation unit's protected A boundaries, collecting them on
  /// first use.  Empty when no collector was supplied.
  llvm::ArrayRef<uint64_t> ProtectedABoundaries() const;

  /// Return whether the observational-irrelevance rule's verdict on one window
  /// is proved not to change the emitted output.
  ///
  /// The rule has two verdicts.  It commits when every enumerated map is
  /// accepted with one concrete output, and that output is
  /// \p soleConcreteOutputKey, the key every realized prefix map shares.
  /// Otherwise it is denied, and when \p legacyProposalRuleReachable is false
  /// no rule remains: the least-source-mutation rule is bounded by the same
  /// realization cost, which the caller has already found over budget, so the
  /// window declines and keeps \p baseMap.  Its output is then the realization
  /// of \p baseMap itself.
  ///
  /// Both verdicts therefore emit one output exactly when that realization is
  /// accepted with \p soleConcreteOutputKey.  This realizes it, once, and
  /// answers so.  Neither verdict feeds a later resolution when
  /// \p laterWindowCarriesAmbiguity is false, since every later window is then
  /// skipped however this one ends; when it is true, or when the legacy rule is
  /// reachable, a denied rule is followed by a verdict this cannot predict, and
  /// the answer is false without realizing anything.
  bool ObservationalVerdictIsOutputNeutral(
      size_t windowIndex, llvm::ArrayRef<int64_t> baseMap,
      llvm::StringRef soleConcreteOutputKey, bool legacyProposalRuleReachable,
      bool laterWindowCarriesAmbiguity) const;

  /// Return whether one certification window can carry alignment ambiguity.
  ///
  /// A forced anchor is, by definition, an edge every optimal path takes. A
  /// window whose A tokens are all forced-matched therefore admits exactly one
  /// optimal map through it: the matched A tokens are maximal, so a competing
  /// optimal map matches the same tokens, and each of those matches is forced
  /// to the same B token. Its unmatched B tokens are pinned by the same
  /// anchors. Resolution can only rediscover that map.
  ///
  /// This is a statement about the window's proof state, not about its cost.
  /// It never passes over a window whose outcome could differ, which is what
  /// separates it from a work budget: a budget declines windows that would have
  /// committed, this declines only windows with nothing to commit. Recomputing
  /// a window's all-optimal pair facts is quadratic in its own rectangle, so
  /// skipping the determined ones is worth stating as a theorem rather than
  /// paying for the same answer.
  bool WindowCarriesAmbiguity(size_t windowIndex) const;

  Dependencies deps_;

  /// Protected A boundaries, collected by `ProtectedABoundaries()` on first
  /// use.  Mutable because `Resolve()` is const: this caches a run constant
  /// and no rule can observe whether it was collected before or after.
  mutable std::optional<std::vector<uint64_t>> protectedABoundaries_;

  /// A ranges of the windows this resolution has committed through the
  /// structure-respecting repair, in commit order.  Mutable because
  /// `Resolve()` is const; it is written only as windows commit and read by
  /// `Simulate()`.
  mutable std::vector<std::pair<uint64_t, uint64_t>> committedTieRanges_;
};

/// One run's recorded alignment-resolution theorem.
///
/// Resolution is a function of run constants alone: the A and B token streams,
/// their gap provenance, the certification byte budget, and the parsed model as
/// it stands before any region has been given up.  The narrowing ladder builds a
/// fresh engine per attempt, and the one engine input that differs between
/// attempts -- the set of owners whose callsite must not be preserved -- is
/// first read during structural dispatch, which runs strictly after token-diff
/// planning has published this theorem.  Every attempt therefore puts the
/// identical question to the resolver, and answering it costs one complete
/// refold of the translation unit per enumerated candidate map.  Recording the
/// answer lets a run pay for it once instead of once per attempt.
///
/// This is a memo, not a budget.  It never declines a window, never shortens an
/// enumeration, and never changes which anchors are committed: what it replays
/// is exactly what the resolver proved.  `MatchesInputs()` re-checks the core
/// facts the answer was proved about, so a caller that reaches the resolver with
/// a different alignment recomputes rather than inheriting a theorem about
/// someone else's stream.
struct AlignmentSemanticResolutionMemo {
  /// Identity of one window in the partition an answer was proved against.
  ///
  /// The rectangle and its certification status are what decide whether a
  /// window is resolved at all and which pairs its enumeration may consider.
  struct WindowIdentity {
    uint64_t aBegin = 0;
    uint64_t aEnd = 0;
    uint64_t bBegin = 0;
    uint64_t bEnd = 0;
    bool certified = false;
  };

  /// Whether `resolution` holds an answer this run already proved.
  bool recorded = false;

  /// The core-theorem facts `resolution` was proved about.
  std::vector<int64_t> forcedMap;
  std::vector<WindowIdentity> certificationWindows;
  size_t aLexemeCount = 0;
  size_t bLexemeCount = 0;

  /// The recorded answer, replayed verbatim whenever the facts still match.
  RefoldAlignmentSemanticResolver::ResolutionResult resolution;

  /// Return whether a recorded answer was proved about exactly these facts.
  ///
  /// The forced map and the certified window partition are what the resolver
  /// enumerates against, so two calls that agree on both, on the same token
  /// stream lengths, are asking one question.  Comparing them is linear; proving
  /// the answer again is a whole-translation-unit refold per candidate.
  bool MatchesInputs(size_t aLexemes, size_t bLexemes,
                     const diffutils::CertifiedLcsResult &alignment) const;

  /// Record \p result as this run's answer for these facts.
  void Record(size_t aLexemes, size_t bLexemes,
              const diffutils::CertifiedLcsResult &alignment,
              RefoldAlignmentSemanticResolver::ResolutionResult result);
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTSEMANTICRESOLVER_H

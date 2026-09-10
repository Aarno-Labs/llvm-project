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
  };

  struct ResolutionResult {
    std::vector<int64_t> selectedMap;
    std::vector<diffutils::LcsAnchorProof> selectedAnchorProofs;
    std::vector<AlignmentSemanticResolutionWitness> witnesses;
    bool committedEquivalentClass = false;
    bool completeEnumeration = false;
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
  /// Enumeration budgets are proof budgets only: exceeding one returns the
  /// forced-only map for that window and cannot authorize a partial class or
  /// ranked winner.
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
    uint64_t acceptedMapCount = 0;
    uint64_t rejectedMapCount = 0;
  };

  /// Resolve the ambiguity inside one certification window.
  ///
  /// Candidate maps differ from `baseMap` only inside the window's A range, so
  /// every simulation observes identical anchors everywhere else and the
  /// comparison isolates this window's choice. `windowIndex` must satisfy
  /// `HasCompleteSemanticOracleForWindow()`.
  ///
  /// The window's complete candidate set is always enumerated and simulated in
  /// full: uniqueness is a property of the whole enumerated set, so committing
  /// on a prefix would be the ranked selection this resolver exists to avoid.
  WindowResolution
  ResolveCertificationWindow(size_t windowIndex,
                             llvm::ArrayRef<int64_t> baseMap) const;

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
  const AlignmentSemanticSimulationResult &RealizeCandidateMap(
      size_t windowIndex, size_t mapIndex, size_t mapCount,
      llvm::ArrayRef<int64_t> candidateMap,
      std::optional<AlignmentSemanticSimulationResult> &slot) const;

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

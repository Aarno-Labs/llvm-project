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
  /// Resolution is per certification window. Every window whose all-optimal
  /// pair facts are available is resolved independently, in source order, and
  /// each committed window contributes its anchors to the base map used by the
  /// next. A window without retained facts, or one whose ambiguity the theorems
  /// below cannot close, keeps exactly its core-forced anchors and does not
  /// prevent an independent window from committing. The single-window
  /// complete-stream case reduces to the historical whole-stream theorem.
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
    uint64_t enumeratedMapCount = 0;
    uint64_t acceptedMapCount = 0;
    uint64_t rejectedMapCount = 0;
    /// Candidate simulations this window ran, charged against the run's work
    /// budget whether or not the window committed.
    size_t simulationsSpent = 0;
  };

  /// Resolve the ambiguity inside one certification window.
  ///
  /// Candidate maps differ from `baseMap` only inside the window's A range, so
  /// every simulation observes identical anchors everywhere else and the
  /// comparison isolates this window's choice. `windowIndex` must satisfy
  /// `HasCompleteSemanticOracleForWindow()`.
  ///
  /// `simulationBudget` is how many candidate simulations the run has left. A
  /// window whose complete candidate set exceeds it is declined outright and
  /// never partially simulated: uniqueness is a property of the whole
  /// enumerated set, so committing on a prefix would be the ranked selection
  /// this resolver exists to avoid.
  WindowResolution
  ResolveCertificationWindow(size_t windowIndex,
                             llvm::ArrayRef<int64_t> baseMap,
                             size_t simulationBudget) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTSEMANTICRESOLVER_H

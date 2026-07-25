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
  std::string sourceGraphOutputs;
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
  /// The current theorem requires one retained complete-stream oracle. A
  /// partitioned or partially certified result therefore returns the
  /// independently certified core map without inspecting unavailable pair
  /// facts; future compositional restoration may relax that gate per window.
  ///
  /// Enumeration budgets are proof budgets only: exceeding one returns the
  /// forced-only map and cannot authorize a partial class or ranked winner.
  ResolutionResult Resolve() const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTSEMANTICRESOLVER_H

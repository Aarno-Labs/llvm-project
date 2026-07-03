//===--- FinalLineControlModel.h -------------------------------*- C++ -*-===//
//
// Final-stream line-control proof and pruning support for clang-refold.
//
// Compact final-line-control proofs, plus executable validation, are the
// pruning authority. Generation sites carry the typed obligation that explains
// why a directive exists, while the fixed-point pruner discharges physical
// deletion only when validation proves that removing that exact final-stream
// range preserves the accepted preprocessed output.
//
// This header also exposes `buildFinalLineControlValidationCallback`, the
// factory that constructs the executable preprocessing oracle the pruner
// consumes for each proposed `#line` deletion.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H

#include "core/RefoldModel.h"

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

class RefoldTheoremAudit;

/// Physical source-owner identity associated with a final-stream line-control
/// fact.
///
/// The final minimizer must not conflate the spelling emitted in a `#line`
/// directive with the owner key used for refold-map/model lookup.  The emitted
/// spelling is logical preprocessor state; this key is the physical source
/// domain that supplied the fact.
struct FinalLineControlOwnerKey {
  std::string physicalFile;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;

  FinalLineControlOwnerKey() = default;
  FinalLineControlOwnerKey(std::string physicalFile,
                           std::optional<uint64_t> ownerIncludeId)
      : physicalFile(std::move(physicalFile)), ownerIncludeId(ownerIncludeId) {}
};

/// Byte-accurate mapping from a final-output slice back to the physical source
/// bytes it copied verbatim.
///
/// Source mappings are still maintained by the engine so downstream
/// materialized ranges can be shifted deterministically after a deletion.  They
/// no longer feed a final-stream observer/layout scanner.
struct FinalLineControlSourceMapping {
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;
  std::string physicalFile;
  uint64_t sourceBegin = 0;
  uint64_t sourceEnd = 0;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
};

/// Canonicalize final-to-source mapping records in deterministic final-byte
/// order.
void CanonicalizeFinalLineControlSourceMappings(
    std::vector<FinalLineControlSourceMapping> &mappings);

/// Adjust final-to-source mapping records after deleting a final-output byte
/// range.
void AdjustFinalLineControlSourceMappingsAfterDeletion(
    std::vector<FinalLineControlSourceMapping> &mappings, uint64_t removedBegin,
    uint64_t removedEnd);

/// One `#line` / line-control directive that exists in the final emitted C
/// stream, identified by the construction site that produced it.
struct FinalLineDirective {
  enum class Origin : uint8_t {
    PreservedSource,
    SyntheticIncludeEntry,
    SyntheticIncludeReturn,
    SyntheticNewlineResync,
    SyntheticSourceLineResume,
    SyntheticTUPrologue,
    SyntheticLayoutBarrier,
    Unknown,
  };
};

/// Return a stable diagnostic spelling for a final-line-control directive
/// origin.
const char *toString(FinalLineDirective::Origin origin);

/// Why a final line-control directive was emitted.
///
/// This is intentionally not a deletion proof.  A directive can have a valid
/// semantic obligation and still be required in the final stream.
enum class FinalLineControlObligation : uint8_t {
  SourceStateRepair,
  IncludeReturnRepair,
  TUPrologueRepair,
  HeaderResumeRepair,
  LayoutBoundaryRepair,
  BuiltinObserverLive,
  CosmeticSyntheticResync,
  DominatedSyntheticDirective,
};

/// Return a stable diagnostic spelling for a final-line-control obligation.
const char *toString(FinalLineControlObligation obligation);

/// Whether deletion of an exact final-stream directive has been discharged.
///
/// `NotProven` is the fail-closed default.  The pruner may turn a validation-
/// accepted candidate into `Removable`, but deletion is never performed solely
/// because an obligation exists.
enum class FinalLineControlRemovalVerdict : uint8_t {
  NotProven,
  Removable,
  Required,
};

/// Return a stable diagnostic spelling for a final-line-control removal
/// verdict.
const char *toString(FinalLineControlRemovalVerdict verdict);

/// The theorem class that discharged physical deletion of a directive.
///
/// Observer/layout discharge values identify diagnostic provenance for
/// already-created artifacts, while `ValidationPreservedEquivalence` is the
/// live authority: after a typed obligation admits the candidate, executable
/// validation proves the exact deletion preserves the accepted `-E -P` output.
enum class FinalLineControlRemovalDischarge : uint8_t {
  None,
  ObserverAndLayoutDead,
  SyntheticIncludeEntryDominated,
  SyntheticNewlineResyncStaleBeforeInclude,
  SyntheticTUPrologueDominatedByRepair,
  ValidationPreservedEquivalence,
};

/// Return a stable diagnostic spelling for a final-line-control removal
/// discharge class.
const char *toString(FinalLineControlRemovalDischarge discharge);

/// Compact proof/provenance record for why a final-stream line-control
/// directive exists.
struct FinalLineControlObligationProof {
  FinalLineControlObligation obligation =
      FinalLineControlObligation::DominatedSyntheticDirective;
  FinalLineDirective::Origin origin = FinalLineDirective::Origin::Unknown;
  std::optional<FinalLineControlOwnerKey> physicalOwner = std::nullopt;
  bool producerProven = false;
};

/// Compact proof/provenance record for whether a final-stream line-control
/// directive may be physically deleted.
struct FinalLineControlRemovalProof {
  FinalLineControlRemovalVerdict verdict =
      FinalLineControlRemovalVerdict::NotProven;
  FinalLineDirective::Origin origin = FinalLineDirective::Origin::Unknown;
  FinalLineControlRemovalDischarge discharge =
      FinalLineControlRemovalDischarge::None;
  std::optional<FinalLineControlOwnerKey> physicalOwner = std::nullopt;
  bool producerProven = false;
};

/// Return whether two optional physical-owner keys describe the same final
/// line-control proof owner.
bool SameFinalLineControlOwner(
    const std::optional<FinalLineControlOwnerKey> &lhs,
    const std::optional<FinalLineControlOwnerKey> &rhs);

/// Return whether two optional obligation proofs are identical for final
/// line-control deduplication.
bool SameFinalLineControlObligationProof(
    const std::optional<FinalLineControlObligationProof> &lhs,
    const std::optional<FinalLineControlObligationProof> &rhs);

/// Return whether two optional removal proofs are identical for final
/// line-control deduplication.
bool SameFinalLineControlRemovalProof(
    const std::optional<FinalLineControlRemovalProof> &lhs,
    const std::optional<FinalLineControlRemovalProof> &rhs);

/// Build a compact obligation proof for one final-stream line-control
/// directive.
FinalLineControlObligationProof MakeFinalLineControlObligationProof(
    FinalLineControlObligation obligation, FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> physicalOwner, bool producerProven);

/// Build a compact removal proof for one final-stream line-control directive.
FinalLineControlRemovalProof MakeFinalLineControlRemovalProof(
    FinalLineControlRemovalVerdict verdict, FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> physicalOwner, bool producerProven,
    FinalLineControlRemovalDischarge discharge =
        FinalLineControlRemovalDischarge::None);

/// Explicit final-line-control pruning candidate.
///
/// The byte range is always in the current final-output coordinate space.  A
/// candidate must carry both compact proof records; otherwise the pruner fails
/// closed and leaves the directive intact.
struct FinalLineControlPruneCandidate {
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;
  FinalLineDirective::Origin origin = FinalLineDirective::Origin::Unknown;
  std::optional<FinalLineControlOwnerKey> physicalOwner = std::nullopt;
  bool producerProven = false;
  std::optional<FinalLineControlObligationProof> obligationProof = std::nullopt;
  std::optional<FinalLineControlRemovalProof> removalProof = std::nullopt;
};

FinalLineControlPruneCandidate MakeFinalLineControlPruneCandidate(
    uint64_t finalBegin, uint64_t finalEnd, FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> physicalOwner, bool producerProven,
    FinalLineControlObligation obligation,
    FinalLineControlRemovalVerdict removalVerdict =
        FinalLineControlRemovalVerdict::NotProven);

/// Return whether a pruning candidate carries both compact proof records
/// required by the fixed-point pruner.
bool HasCompleteFinalLineControlProof(
    const FinalLineControlPruneCandidate &candidate);

/// Removed final-output range.  Ranges are reported in the coordinate space
/// that existed when the deletion was performed, matching the engine's existing
/// source-mapping adjustment convention.
struct FinalLineControlRemovedRange {
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;
};

/// Final-line-control authority contract.
///
/// Deletion is driven by compact obligations, fixed-point candidate ordering,
/// and executable validation of the exact candidate deletion.
struct FinalLineControlAuthorityContract {
  bool compactRemovalProofIsAuthoritative = true;
  bool fixedPointPruningIsAuthoritative = true;
  bool validationCallbackIsAuthoritative = true;

  bool IsClosedUnderCompactProofs() const {
    return compactRemovalProofIsAuthoritative &&
           fixedPointPruningIsAuthoritative &&
           validationCallbackIsAuthoritative;
  }
};

/// Return the static final-line-control pruning authority contract.
FinalLineControlAuthorityContract getFinalLineControlAuthorityContract();

/// Result of one deterministic fixed-point final-line-control pruning run.
struct FinalLineControlPruneResult {
  std::string output;
  uint32_t iterations = 0;
  bool changed = false;
  std::vector<FinalLineControlRemovedRange> removedRanges;
  FinalLineControlAuthorityContract authority;
};

/// Optional executable oracle for a proposed final-stream #line deletion.
using FinalLineControlValidationCallback =
    std::function<bool(llvm::StringRef currentOutput,
                       llvm::StringRef candidateOutput, std::string &reason)>;

/// Run deterministic fixed-point pruning over explicit final-stream
/// line-control candidates.
///
/// This pass replaces the passive final observer/layout scanner.  A candidate
/// is attempted only when it carries both compact proof records, has a stable
/// current byte range, and is not explicitly Required.  The pass removes at
/// most one validation-accepted directive per iteration, then shifts the
/// remaining candidate ranges and starts over.
FinalLineControlPruneResult PruneFinalLineControlDirectives(
    llvm::StringRef finalSource,
    llvm::ArrayRef<FinalLineControlPruneCandidate> removableCandidates =
        llvm::ArrayRef<FinalLineControlPruneCandidate>(),
    FinalLineControlValidationCallback validationCallback =
        FinalLineControlValidationCallback());

/// Build the executable oracle that validates one proposed final-stream
/// `#line` deletion.
///
/// The returned callback is consumed by `RefoldEngine::Refold` and ultimately
/// by the final-line-control pruner.  It re-invokes the producer-recorded
/// preprocessor on the current accepted final source and the candidate final
/// source using one stable temporary path located beside \p outputPath, so
/// `__FILE__` and quoted-include lookup remain comparable across both inputs.
/// Byte-for-byte preprocessor equivalence is accepted first; otherwise the
/// callback falls back to token-sequence equality, which is the same oracle
/// `--check` uses.
FinalLineControlValidationCallback buildFinalLineControlValidationCallback(
    llvm::StringRef outputPath, const RefoldModel::PreprocessContext &ctx);

/// Audit the authority contract returned by the final-line-control pruner.
///
/// Reports a no-legacy audit finding for each pillar of the authority contract
/// (compact removal proof, fixed-point pruning, validation callback) that is
/// not marked authoritative.  Returns the contract's
/// `IsClosedUnderCompactProofs` verdict so the caller can fail closed when a
/// strict run has lost authority over the final pruning pass.
bool AuditFinalLineControlAuthorityContract(
    const RefoldTheoremAudit &audit,
    const FinalLineControlAuthorityContract &authority, llvm::StringRef role);

/// Audit the compact-proof population of \p candidates before fixed-point
/// pruning runs.
///
/// Counts candidates that lack a complete obligation/removal proof and reports
/// one no-legacy finding when any are missing.  Returns true iff every
/// candidate carries the compact proof records the pruner needs.
bool AuditFinalLineControlRemovalProofPopulation(
    const RefoldTheoremAudit &audit,
    llvm::ArrayRef<FinalLineControlPruneCandidate> candidates,
    llvm::StringRef role);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H

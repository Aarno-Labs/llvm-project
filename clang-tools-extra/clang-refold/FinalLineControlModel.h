//===--- FinalLineControlModel.h -------------------------------*- C++ -*-===//
//
// Final-stream line-control proof scaffolding for clang-refold.
//
// Compact final-line-control proofs, plus executable validation, are the
// pruning authority. The old observer/layout/physical-line scanner has been
// removed from this interface: generation sites now carry the typed obligation
// that explains why a directive exists, while the fixed-point pruner discharges
// physical deletion only when validation proves that removing that exact final-
// stream range preserves the accepted preprocessed output.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H

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
      : physicalFile(std::move(physicalFile)),
        ownerIncludeId(ownerIncludeId) {}
};

/// Byte-accurate mapping from a final-output slice back to the physical source
/// bytes it copied verbatim.
///
/// Source mappings are still maintained by the engine so downstream materialized
/// ranges can be shifted deterministically after a deletion.  They no longer
/// feed a final-stream observer/layout scanner.
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

const char *toString(FinalLineControlRemovalVerdict verdict);

/// The theorem class that discharged physical deletion of a directive.
///
/// The legacy observer/layout discharges remain as historical diagnostic values
/// for already-created artifacts, but `ValidationPreservedEquivalence` is the
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

bool SameFinalLineControlOwner(
    const std::optional<FinalLineControlOwnerKey> &lhs,
    const std::optional<FinalLineControlOwnerKey> &rhs);

bool SameFinalLineControlObligationProof(
    const std::optional<FinalLineControlObligationProof> &lhs,
    const std::optional<FinalLineControlObligationProof> &rhs);

bool SameFinalLineControlRemovalProof(
    const std::optional<FinalLineControlRemovalProof> &lhs,
    const std::optional<FinalLineControlRemovalProof> &rhs);

FinalLineControlObligationProof MakeFinalLineControlObligationProof(
    FinalLineControlObligation obligation, FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> physicalOwner, bool producerProven);

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
    uint64_t finalBegin, uint64_t finalEnd,
    FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> physicalOwner, bool producerProven,
    FinalLineControlObligation obligation,
    FinalLineControlRemovalVerdict removalVerdict =
        FinalLineControlRemovalVerdict::NotProven);

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
/// The legacy observer/layout scanner is no longer authoritative.  Deletion is
/// driven by compact obligations, fixed-point candidate ordering, and executable
/// validation of the exact candidate deletion.
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

FinalLineControlAuthorityContract GetFinalLineControlAuthorityContract();

struct FinalLineControlPruneResult {
  std::string output;
  uint32_t iterations = 0;
  bool changed = false;
  std::vector<FinalLineControlRemovedRange> removedRanges;
  FinalLineControlAuthorityContract authority;
};

/// Optional executable oracle for a proposed final-stream #line deletion.
using FinalLineControlValidationCallback = std::function<bool(
    llvm::StringRef currentOutput, llvm::StringRef candidateOutput,
    std::string &reason)>;

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

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H

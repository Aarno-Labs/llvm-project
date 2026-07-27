//===--- RefoldAlignmentDiagnostic.h ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Owning evidence records for weighted-LCS ambiguity diagnostics.
//
// These types preserve the physical identity behind every protected alignment
// boundary and the exact certification facts for one ambiguity window. They
// are deliberately separate from production partition coordinates: proof
// scheduling may deduplicate a numeric A frontier, while diagnostics retain one
// identity per physical construct and boundary role.
//
// The records contain no borrowed views. Collectors may build them locally and
// publish them by const reference without tying their lifetime to token or
// producer buffers. They are evidence-only and grant no alignment authority.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTDIAGNOSTIC_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTDIAGNOSTIC_H

#include "source/DiffAlgorithms.h"
#include "source/RefoldPreprocessingStructureIndex.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

/// Physical role by which a protected construct contributes an A frontier.
///
/// A tokenless structure interval projects to one `StructureBoundary`. An
/// include directive contributes two independent identities because its
/// preprocessed cover begin and cover end protect different DP seams.
/// `SourceOwnerUnavailable` is an evidence-only owner census record: it
/// reports that one represented header occurrence could not be indexed and is
/// never forwarded to the LCS boundary-projection collector.
enum class AlignmentBoundaryRole : uint8_t {
  StructureBoundary,
  IncludeCoverBegin,
  IncludeCoverEnd,
  SourceOwnerUnavailable,
};

using AlignmentWindowAnchorKind =
    diffutils::LcsDiagnosticWindowAnchorKind;
using AlignmentWindowAnchor = diffutils::LcsDiagnosticWindowAnchor;

/// Identity of one physical protected-boundary diagnostic obligation.
///
/// `identityId` belongs to the diagnostic ledger and is distinct from producer
/// ids. `aBoundary` is absent and `projectionComplete` is false when exact
/// tokmap/include-cover evidence cannot project the physical interval to one
/// A-token frontier. Multiple identities may intentionally carry the same
/// numeric frontier. `incompleteEvidenceReason` is populated only when the
/// identity itself could not be constructed or projected exactly; it is
/// evidence-only and has no production proof effect.
struct AlignmentProtectedBoundaryIdentity {
  uint64_t identityId = 0;
  std::optional<uint64_t> aBoundary;
  bool projectionComplete = false;

  std::string sourcePath;
  uint64_t sourceBegin = 0;
  uint64_t sourceEnd = 0;

  PreprocessingStructureKind structureKind =
      PreprocessingStructureKind::OtherDirective;
  PreprocessingStructureModelKind modelKind =
      PreprocessingStructureModelKind::None;

  std::optional<uint64_t> modelItemId;
  std::optional<uint64_t> ownerIncludeId;
  std::optional<uint64_t> ownerConditionalArmId;
  std::optional<uint64_t> conditionalGroupId;
  std::optional<uint64_t> conditionalArmId;

  AlignmentBoundaryRole role = AlignmentBoundaryRole::StructureBoundary;
  std::string incompleteEvidenceReason;
};

/// Separate production and diagnostic views of protected A boundaries.
///
/// `proofSchedulingCoordinates` is sorted and unique because partition
/// certification consumes only numeric A frontiers. `diagnosticIdentities`
/// retains one canonically ordered entry per physical construct and boundary
/// role, including entries that share a frontier or have no exact projection.
/// The identity ledger is evidence-only, may be omitted outside trace mode,
/// and must never choose or order proof partitions.
struct AlignmentProtectedBoundarySurfaces {
  std::vector<uint64_t> proofSchedulingCoordinates;
  std::vector<AlignmentProtectedBoundaryIdentity> diagnosticIdentities;
};

using AlignmentAdmissiblePair = diffutils::LcsDiagnosticAdmissiblePair;
using AlignmentProtectedBoundaryProjection =
    diffutils::LcsProtectedBoundaryDiagnosticProjection;
using AlignmentAmbiguityWindow =
    diffutils::LcsAmbiguityWindowDiagnosticRecord;

/// Install the physical protected-boundary identity request for one run.
///
/// Only stable ids and exact A projections cross into the LCS layer. The
/// complete source/model identity remains in `protectedBoundaries`, while the
/// certifier records one projection result per id as its local DP is live.
void initializeAlignmentDiagnosticEvidence(
    const AlignmentProtectedBoundarySurfaces &protectedBoundaries,
    diffutils::LcsCertificationDiagnosticEvidence &diagnosticEvidence);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDALIGNMENTDIAGNOSTIC_H

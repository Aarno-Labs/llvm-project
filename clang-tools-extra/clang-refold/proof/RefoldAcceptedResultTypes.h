//===--- RefoldAcceptedResultTypes.h ---------------------------*- C++ -*-===//
//
// Accepted-result proof lattice carrier records for clang-refold.
//
// These namespace-level proof carriers describe accepted patch candidates,
// theorem summaries, witness metadata, and selection bookkeeping without tying
// the proof vocabulary to RefoldEngine private state.
//
// The carriers themselves now live in the eight headers included below, split
// out because a single 2,400-line header made every proof concept visible in
// every translation unit that needed any one of them.  The include order here
// is the dependency order, and it is strict: each header depends only on ones
// listed above it, so a layer cannot quietly acquire a carrier from a layer
// above.
//
//   RefoldTheoremTypes         emission path, theorem class, ranking buckets
//   RefoldAcceptancePathTypes  construction provenance for an acceptance path
//   RefoldCompletenessTypes    completeness and theorem-domain contracts
//   RefoldAnchorWitnessTypes   TU / include / owner-realization anchors
//   RefoldTilingWitnessTypes   structural hunk tiling partitions
//   RefoldProofDischargeTypes  obligation discharge and the emitted proof
//   RefoldCandidateTypes       ProofSummary, AcceptedResultCandidate, selectors
//   RefoldMacroPatchTypes      macro-patch proof kinds and certificates
//
// This header remains as the umbrella so existing consumers keep compiling
// unchanged.  A consumer that needs only one layer should include that layer
// directly rather than this file; narrowing those includes is what actually
// removes the unwanted dependencies, and it is deliberately not done here so
// the split itself stays a provably behavior-preserving change.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTTYPES_H

#include "proof/RefoldTheoremTypes.h"
#include "proof/RefoldAcceptancePathTypes.h"
#include "proof/RefoldCompletenessTypes.h"
#include "proof/RefoldAnchorWitnessTypes.h"
#include "proof/RefoldTilingWitnessTypes.h"
#include "proof/RefoldProofDischargeTypes.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldMacroPatchTypes.h"

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTTYPES_H

//===--- RefoldProofServices.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Composition of clang-refold's accepted-result proof services.
//
// `RefoldProofServices` constructs the proof services that depend on one
// another, in an order where every service is built after everything it
// borrows, so no service needs a callback or a late-bound pointer to reach a
// peer.  It owns them and nothing else: it has no proof behavior of its own.
//
// Only the composition root may name this type.  Feature code borrows the
// individual services it uses, by reference, through its own constructor or
// `Dependencies` bundle.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFSERVICES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFSERVICES_H

#include "proof/RefoldAcceptancePathClassifier.h"
#include "proof/RefoldAcceptedCandidateBuilder.h"
#include "proof/RefoldAcceptedResultRanker.h"
#include "proof/RefoldMacroPatchProofClassifier.h"
#include "proof/RefoldOwnerRealizationProofBuilder.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTilingWitnessTypes.h"
#include "proof/RefoldWitnessEquivalenceKeyBuilder.h"
#include "proof/RefoldWitnessResolver.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <vector>

namespace clang {
namespace refold {

class RefoldArgTextRecovery;
class RefoldMacroTopology;
class RefoldModel;
class RefoldOwnerStateProof;
class RefoldProofSummaryBuilder;
class RefoldSourceMapper;
class RefoldTUEditPlanner;
class RefoldTheoremAudit;
class RefoldTokenTextAnalysis;

/// Owner of the mutually dependent accepted-result proof services.
class RefoldProofServices {
public:
  /// Construct every proof service from the borrowed run inputs.
  ///
  /// \p proofSummaryBuilder and \p theoremAudit are constructed before this
  /// object because the audit itself borrows the summary builder.  The
  /// mixed-owner tiling vectors are per-pass state owned by the caller and
  /// read by the owner-realization builder as tiling witnesses accumulate.
  RefoldProofServices(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> bToks, RefoldSourceMapper &sourceMapper,
      const RefoldTokenTextAnalysis &tokenText,
      const RefoldArgTextRecovery &argTextRecovery,
      const RefoldMacroTopology &macroTopology,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldTUEditPlanner &tuEdits,
      const RefoldProofSummaryBuilder &proofSummaryBuilder,
      const RefoldTheoremAudit &theoremAudit,
      TheoremAuditStats &lastTheoremAudit, bool strict,
      ProofAuditMode &proofAuditMode,
      const bool &alignmentSemanticTheoremActive,
      const std::vector<MixedOwnerTilingSegmentBinding>
          &mixedOwnerTilingSegmentBindings,
      const std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses);

  const RefoldWitnessTrace &WitnessTrace() const { return witnessTrace_; }
  const RefoldWitnessEquivalenceKeyBuilder &EquivalenceKeyBuilder() const {
    return equivalenceKeyBuilder_;
  }
  const RefoldWitnessResolver &WitnessResolver() const {
    return witnessResolver_;
  }
  const RefoldAcceptedResultRanker &AcceptedResultRanker() const {
    return acceptedResultRanker_;
  }
  const RefoldAcceptancePathClassifier &AcceptancePathClassifier() const {
    return acceptancePathClassifier_;
  }
  const RefoldOwnerRealizationProofBuilder &
  OwnerRealizationProofBuilder() const {
    return ownerRealizationProofBuilder_;
  }
  const RefoldMacroPatchProofClassifier &MacroPatchProofClassifier() const {
    return macroPatchProofClassifier_;
  }
  const RefoldAcceptedCandidateBuilder &AcceptedCandidateBuilder() const {
    return acceptedCandidateBuilder_;
  }

private:
  // Declaration order is construction order, and it is load-bearing: each
  // service holds a direct reference to every service declared above it that
  // it uses.
  RefoldWitnessTrace witnessTrace_;
  RefoldWitnessEquivalenceKeyBuilder equivalenceKeyBuilder_;
  RefoldWitnessResolver witnessResolver_;
  RefoldAcceptedResultRanker acceptedResultRanker_;
  RefoldAcceptancePathClassifier acceptancePathClassifier_;
  RefoldOwnerRealizationProofBuilder ownerRealizationProofBuilder_;
  RefoldMacroPatchProofClassifier macroPatchProofClassifier_;
  RefoldAcceptedCandidateBuilder acceptedCandidateBuilder_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFSERVICES_H

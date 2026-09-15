//===--- RefoldProofServices.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Construction of clang-refold's accepted-result proof services.  Every
// dependency below is a direct reference to a service built earlier, here or
// by the caller; none is a callback.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldProofServices.h"

#include <cstdint>

using namespace llvm;

namespace clang {
namespace refold {

RefoldProofServices::RefoldProofServices(
    const RefoldModel &model, StringRef bSource, ArrayRef<PPTok> bToks,
    RefoldSourceMapper &sourceMapper, const RefoldTokenTextAnalysis &tokenText,
    const RefoldArgTextRecovery &argTextRecovery,
    const RefoldMacroTopology &macroTopology,
    const RefoldOwnerStateProof &ownerStateProof,
    const RefoldTUEditPlanner &tuEdits,
    const RefoldProofSummaryBuilder &proofSummaryBuilder,
    const RefoldTheoremAudit &theoremAudit, TheoremAuditStats &lastTheoremAudit,
    const RefoldWitnessTrace &witnessTrace,
    const std::vector<MixedOwnerTilingSegmentBinding>
        &mixedOwnerTilingSegmentBindings,
    const std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses)
    : witnessTrace_(witnessTrace),
      equivalenceKeyBuilder_(RefoldWitnessEquivalenceKeyBuilder::Dependencies{
          sourceMapper, bSource, bToks}),
      witnessResolver_(RefoldWitnessResolver::Dependencies{
          witnessTrace_, equivalenceKeyBuilder_, theoremAudit}),
      acceptedResultRanker_(RefoldAcceptedResultRanker::Dependencies{
          witnessTrace_, witnessResolver_, theoremAudit, lastTheoremAudit,
          proofSummaryBuilder}),
      acceptancePathClassifier_(RefoldAcceptancePathClassifier::Dependencies{
          model, proofSummaryBuilder}),
      ownerRealizationProofBuilder_(
          RefoldOwnerRealizationProofBuilder::Dependencies{
              model, ownerStateProof, tuEdits,
              static_cast<uint64_t>(bToks.size()), proofSummaryBuilder,
              acceptedResultRanker_, mixedOwnerTilingSegmentBindings,
              mixedOwnerTilingWitnesses, acceptancePathClassifier_}),
      macroPatchProofClassifier_(RefoldMacroPatchProofClassifier::Dependencies{
          macroTopology, ownerStateProof, proofSummaryBuilder,
          ownerRealizationProofBuilder_, acceptancePathClassifier_}),
      acceptedCandidateBuilder_(RefoldAcceptedCandidateBuilder::Dependencies{
          model, tokenText, argTextRecovery, macroTopology, sourceMapper, bToks,
          theoremAudit, witnessTrace_, witnessResolver_, acceptedResultRanker_,
          proofSummaryBuilder, ownerRealizationProofBuilder_,
          macroPatchProofClassifier_, acceptancePathClassifier_}) {}

} // namespace refold
} // namespace clang

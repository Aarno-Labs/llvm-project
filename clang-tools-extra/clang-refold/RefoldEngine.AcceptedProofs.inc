//===--- RefoldEngine.AcceptedProofs.inc ------------------------*- C++ -*-===//
//
// `RefoldEngine` member definitions physically split out of
// `RefoldEngine.cpp` to reduce the size of the main implementation file.
//
// This fragment contains members that operate on accepted refolding results
// after candidate construction has already succeeded. The methods here are
// responsible for validating, summarizing, formatting, and materializing
// proof-backed accepted outcomes, including whole-cover replacement planning
// derived from those accepted proofs.
//
// This is a physical extraction only:
//   - the methods remain `RefoldEngine` members
//   - the file is included directly into `RefoldEngine.cpp`
//   - private access, translation-unit scope, and behavior are unchanged
//
// The purpose of this fragment is organizational only: keep accepted-result
// proof handling and its supporting output/materialization logic out of the
// main engine file while preserving the exact implementation structure and
// semantics.
//
//===----------------------------------------------------------------------===//

bool RefoldEngine::MacroInvocationHasWellFormedPasteWitnesses(
    const RefoldModel::MacroInvocation &m) const {
  // Step 6 consumes the exact `paste_tokens` witnesses serialized in Step 5.
  // The producer records the argument-derived fragments of each pasted token,
  // but it does not need to emit the literal glue bytes that came from the
  // macro body itself. For example, `X##_##Y` may serialize only the `X` and
  // `Y` slices while leaving the `_` as an uncovered gap in the final spelling.
  //
  // The witness stream is therefore usable when each recorded fragment is
  // ordered, non-overlapping, non-empty, and stays within the pasted token's
  // spelling, and when every argument-derived fragment names a valid formal.
  // Requiring a contiguous partition of the final spelling would incorrectly
  // reject perfectly valid producer witnesses for common paste patterns.
  //
  // There is one additional producer-side case that must also be treated as
  // witness-backed: a parent invocation can carry `pasteSpans` that are only
  // propagated child-paste contributors inside a standard occurrence of the
  // same parent formal. Those spans are validated at the child/current-surface
  // level by the existing args-only safety gates and do not require a direct
  // parent-level `paste_tokens` decomposition.
  auto hasOnlyPropagatedChildPasteSpans = [&]() -> bool {
    if (m.pasteSpans.empty())
      return false;

    DenseMap<uint32_t, SmallVector<std::pair<uint64_t, uint64_t>, 4>>
        standardOccByteRangesByArg;
    for (const auto &occ : m.argSpans) {
      if (occ.kind != PPArgSpanKind::Standard || !occ.ppByteBegin ||
          !occ.ppByteEnd)
        continue;
      standardOccByteRangesByArg[occ.argIdx].push_back(
          {static_cast<uint64_t>(*occ.ppByteBegin),
           static_cast<uint64_t>(*occ.ppByteEnd)});
    }

    bool sawPasteSpan = false;
    for (const auto &ps : m.pasteSpans) {
      sawPasteSpan = true;
      if (!ps.ppByteBegin || !ps.ppByteEnd)
        return false;

      auto stdIt = standardOccByteRangesByArg.find(ps.argIdx);
      if (stdIt == standardOccByteRangesByArg.end())
        return false;

      const std::pair<uint64_t, uint64_t> spanBytes = {
          static_cast<uint64_t>(*ps.ppByteBegin),
          static_cast<uint64_t>(*ps.ppByteEnd)};
      if (!llvm::is_contained(stdIt->second, spanBytes))
        return false;
    }

    return sawPasteSpan;
  };

  if (m.pasteTokens.empty())
    return hasOnlyPropagatedChildPasteSpans();

  llvm::SmallDenseSet<uint32_t, 8> pasteSpanArgIndices;
  for (const auto &ps : m.pasteSpans)
    pasteSpanArgIndices.insert(ps.argIdx);

  for (const auto &tok : m.pasteTokens) {
    if (tok.spelling.empty() || tok.parts.empty())
      return false;

    uint32_t prevEnd = 0;
    bool sawArgDerivedPart = false;
    for (const auto &part : tok.parts) {
      if (part.byteEnd <= part.byteBegin || part.byteEnd > tok.spelling.size())
        return false;

      // Parts must remain in producer order and may be adjacent or separated by
      // literal macro-body glue, but they must never overlap or move backward.
      if (part.byteBegin < prevEnd)
        return false;

      if (part.argIndex) {
        if (*part.argIndex >= m.defParams.size())
          return false;
        if (!pasteSpanArgIndices.empty() &&
            !pasteSpanArgIndices.count(*part.argIndex))
          return false;
        sawArgDerivedPart = true;
      }

      prevEnd = part.byteEnd;
    }

    // A pasted-token witness that never attributes any bytes back to a formal
    // is not useful for args-only paste preservation.
    if (!sawArgDerivedPart)
      return false;
  }

  return true;
}

RefoldEngine::ProofDischargeRecord
RefoldEngine::ValidateInvocationPreservingProof(const MacroPatch &patch) const {
  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory =
      InventoryMacroPatchAcceptancePath(patch);

  discharge.Require(inventory.currentPath != AcceptedPathKind::Unknown,
                    ProofObligationKind::AcceptedPathClassified,
                    ProofFailureReason::MissingAcceptedPathClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);
  discharge.Require(patch.proofValidated,
                    ProofObligationKind::LegacyValidationRecorded,
                    ProofFailureReason::MissingLegacyValidation);
  discharge.Require(patch.structurePreserving,
                    ProofObligationKind::StructureMatchesAcceptedClass,
                    ProofFailureReason::StructuralMismatch);
  discharge.Require(patch.proofRootMacroId != 0,
                    ProofObligationKind::ProofRootTracked,
                    ProofFailureReason::MissingProofRoot);

  const RefoldModel::MacroInvocation *root =
      patch.proofRootMacroId ? FindMacroInvocationById(patch.proofRootMacroId)
                             : nullptr;
  discharge.Require(root != nullptr,
                    ProofObligationKind::MacroProofRootResolved,
                    ProofFailureReason::MissingMacroProofRootResolution);
  if (root) {
    discharge.Require(GetRootMacroId(root->id) == root->id,
                      ProofObligationKind::MacroProofRootIsTopLevel,
                      ProofFailureReason::NonTopLevelMacroProofRoot);
  }

  switch (patch.proofKind) {
  case MacroPatchProofKind::ArgsOnlyStandard:
    // Standard args-only rewrites do not rely on producer-side paste
    // witnesses; the common preserving obligations above are sufficient.
    break;

  case MacroPatchProofKind::ArgsOnlyPasteSingle:
  case MacroPatchProofKind::ArgsOnlyPasteMulti:
  case MacroPatchProofKind::ArgsOnlyPurePasteOnly: {
    // Paste-preserving args-only classes can be discharged from either of the
    // two deterministic proof sources the builder already relies on today:
    //
    // * a usable producer-side root `paste_tokens` witness stream
    // * or a direct replay proof recorded on the accepted patch after the
    //   rewritten invocation arguments were shown to reconstruct the edited
    //   B-side pasted tokens exactly
    //
    // Wrapper/variadic chains such as XSTR(XCAT(...)) legitimately use the
    // second source when the root invocation carries propagated paste spans
    // but no standalone root-level `paste_tokens` decomposition.
    discharge.Require(root && !root->pasteSpans.empty(),
                      ProofObligationKind::MacroPasteWitnessPresent,
                      ProofFailureReason::MissingPasteWitness);
    if (root && !root->pasteSpans.empty()) {
      const bool hasProducerWitness =
          MacroInvocationHasWellFormedPasteWitnesses(*root);
      discharge.Require(hasProducerWitness || patch.pasteReplayValidated,
                        ProofObligationKind::MacroPasteWitnessWellFormed,
                        ProofFailureReason::MalformedPasteWitness);
    }
    break;
  }

  case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    // Paired pure insertion is only valid on non-paste direct arg/stringify
    // surfaces. The builder already enforces that; Step 6 records it.
    discharge.Require(root && root->pasteSpans.empty(),
                      ProofObligationKind::MacroPasteFreeSurfaceTracked,
                      ProofFailureReason::UnexpectedPasteSurface);
    break;

  case MacroPatchProofKind::DagSubtreeRoot:
    // DAG-preserving rewrites must carry the explicit subtree certificate that
    // Step 5 started recording on accepted root patches.
    discharge.Require(patch.subtreeCertBacked,
                      ProofObligationKind::MacroSubtreeCertificateTracked,
                      ProofFailureReason::MissingSubtreeCertificate);
    if (patch.subtreeCertBacked) {
      discharge.Require(patch.subtreeAdmissible,
                        ProofObligationKind::SubtreeAdmissibilityTracked,
                        ProofFailureReason::MissingSubtreeAdmissibility);
    }
    break;

  case MacroPatchProofKind::CallChainSuffix:
    // Call-chain suffix rewrites are emitted directly on the root callsite
    // slice, so the patch's owning macro id must already be that root.
    discharge.Require(patch.macroId == patch.proofRootMacroId,
                      ProofObligationKind::MacroCallChainWitnessTracked,
                      ProofFailureReason::MissingCallChainWitness);
    break;

  case MacroPatchProofKind::CounterLiteral:
  case MacroPatchProofKind::WholeCoverRealization:
  case MacroPatchProofKind::Unknown:
    break;
  }

  return discharge.Finish();
}

RefoldEngine::ProofDischargeRecord
RefoldEngine::ValidateEmittedInvocationPreservingProof(
    const MacroPatch &patch) const {
  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory =
      InventoryMacroPatchAcceptancePath(patch);

  discharge.Require(inventory.currentPath != AcceptedPathKind::Unknown,
                    ProofObligationKind::AcceptedPathClassified,
                    ProofFailureReason::MissingAcceptedPathClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);
  discharge.Require(patch.proofValidated,
                    ProofObligationKind::LegacyValidationRecorded,
                    ProofFailureReason::MissingLegacyValidation);
  discharge.Require(patch.structurePreserving,
                    ProofObligationKind::StructureMatchesAcceptedClass,
                    ProofFailureReason::StructuralMismatch);
  discharge.Require(patch.proofRootMacroId != 0,
                    ProofObligationKind::ProofRootTracked,
                    ProofFailureReason::MissingProofRoot);

  const RefoldModel::MacroInvocation *root =
      patch.proofRootMacroId ? FindMacroInvocationById(patch.proofRootMacroId)
                             : nullptr;
  discharge.Require(root != nullptr,
                    ProofObligationKind::MacroProofRootResolved,
                    ProofFailureReason::MissingMacroProofRootResolution);

  switch (patch.proofKind) {
  case MacroPatchProofKind::ArgsOnlyStandard:
    break;

  case MacroPatchProofKind::ArgsOnlyPasteSingle:
  case MacroPatchProofKind::ArgsOnlyPasteMulti:
  case MacroPatchProofKind::ArgsOnlyPurePasteOnly: {
    discharge.Require(root && !root->pasteSpans.empty(),
                      ProofObligationKind::MacroPasteWitnessPresent,
                      ProofFailureReason::MissingPasteWitness);
    if (root && !root->pasteSpans.empty()) {
      const bool hasProducerWitness =
          MacroInvocationHasWellFormedPasteWitnesses(*root);
      discharge.Require(hasProducerWitness || patch.pasteReplayValidated,
                        ProofObligationKind::MacroPasteWitnessWellFormed,
                        ProofFailureReason::MalformedPasteWitness);
    }
    break;
  }

  case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    discharge.Require(root && root->pasteSpans.empty(),
                      ProofObligationKind::MacroPasteFreeSurfaceTracked,
                      ProofFailureReason::UnexpectedPasteSurface);
    break;

  case MacroPatchProofKind::DagSubtreeRoot:
    discharge.Require(patch.subtreeCertBacked,
                      ProofObligationKind::MacroSubtreeCertificateTracked,
                      ProofFailureReason::MissingSubtreeCertificate);
    if (patch.subtreeCertBacked) {
      discharge.Require(patch.subtreeAdmissible,
                        ProofObligationKind::SubtreeAdmissibilityTracked,
                        ProofFailureReason::MissingSubtreeAdmissibility);
    }
    break;

  case MacroPatchProofKind::CallChainSuffix:
    discharge.Require(patch.macroId == patch.proofRootMacroId,
                      ProofObligationKind::MacroCallChainWitnessTracked,
                      ProofFailureReason::MissingCallChainWitness);
    break;

  case MacroPatchProofKind::CounterLiteral:
  case MacroPatchProofKind::WholeCoverRealization:
  case MacroPatchProofKind::Unknown:
    break;
  }

  return discharge.Finish();
}


RefoldEngine::ProofDischargeRecord
RefoldEngine::ValidateInvocationRealizationProof(const MacroPatch &patch) const {
  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory = InventoryMacroPatchAcceptancePath(patch);

  discharge.Require(inventory.currentPath != AcceptedPathKind::Unknown,
                    ProofObligationKind::AcceptedPathClassified,
                    ProofFailureReason::MissingAcceptedPathClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);
  discharge.Require(patch.proofRootMacroId != 0,
                    ProofObligationKind::ProofRootTracked,
                    ProofFailureReason::MissingProofRoot);

  if (patch.proofKind == MacroPatchProofKind::WholeCoverRealization) {
    const bool boundsTracked = patch.wholeCoverALo <= patch.wholeCoverAHi &&
                               patch.wholeCoverBRawLo <= patch.wholeCoverBRawHi &&
                               patch.wholeCoverBAdjLo <= patch.wholeCoverBAdjHi;
    const bool boundaryAccountingTracked = boundsTracked &&
                                           patch.wholeCoverBRawLo <= patch.wholeCoverBAdjLo &&
                                           patch.wholeCoverBAdjHi <= patch.wholeCoverBRawHi;
    discharge.Require(patch.proofValidated,
                      ProofObligationKind::LegacyValidationRecorded,
                      ProofFailureReason::MissingLegacyValidation);
    discharge.Require(!patch.structurePreserving,
                      ProofObligationKind::StructureMatchesAcceptedClass,
                      ProofFailureReason::StructuralMismatch);
    discharge.Require(boundsTracked,
                      ProofObligationKind::WholeCoverBoundsTracked,
                      ProofFailureReason::MissingWholeCoverBounds);
    discharge.Require(patch.wholeCoverSelfContained ||
                          patch.wholeCoverNestedSelfContained,
                      ProofObligationKind::WholeCoverContainmentTracked,
                      ProofFailureReason::MissingWholeCoverContainment);
    discharge.Require(boundaryAccountingTracked,
                      ProofObligationKind::WholeCoverBoundaryAccountingTracked,
                      ProofFailureReason::MissingWholeCoverBoundaryAccounting);
  } else {
    discharge.Require(patch.proofValidated,
                      ProofObligationKind::LegacyValidationRecorded,
                      ProofFailureReason::MissingLegacyValidation);
    discharge.Require(!patch.structurePreserving,
                      ProofObligationKind::StructureMatchesAcceptedClass,
                      ProofFailureReason::StructuralMismatch);
  }

  return discharge.Finish();
}

RefoldEngine::ProofDischargeRecord
RefoldEngine::ValidateIncludePreservingProof(AcceptedPathKind currentPath,
                                             const IncludePatch *patch,
                                             const IncludeAnchorWitness *witness) const {
  if (currentPath == AcceptedPathKind::IncludePatchPendingMaterialization) {
    // Pending include materialization is internal-only after step 3. If some
    // caller still tries to validate it as a theorem-facing include path, fail
    // closed immediately rather than reporting a live transitional discharge
    // state.
    ProofDischargeAccumulator discharge;
    discharge.Fail(ProofObligationKind::IncludePendingMaterializationClassified,
                   ProofFailureReason::PendingMaterialization);
    return discharge.Finish();
  }

  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory = BuildAcceptancePathInventory(currentPath);
  discharge.Require(inventory.currentPath != AcceptedPathKind::Unknown,
                    ProofObligationKind::AcceptedPathClassified,
                    ProofFailureReason::MissingAcceptedPathClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);
  discharge.Require(patch != nullptr,
                    ProofObligationKind::IncludePatchShapeTracked,
                    ProofFailureReason::MissingIncludePatchShape);
  if (!patch)
    return discharge.Finish();

  discharge.Require(witness &&
                        witness->evidence != IncludeAnchorEvidenceKind::Unknown,
                    ProofObligationKind::IncludeAnchorWitnessTracked,
                    ProofFailureReason::MissingIncludeAnchorWitness);

  switch (currentPath) {
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
    discharge.Require(patch->aStart < patch->aEnd,
                      ProofObligationKind::IncludeMappedHeaderRangeTracked,
                      ProofFailureReason::MissingMappedHeaderRange);
    discharge.Require(witness &&
                          witness->evidence ==
                              IncludeAnchorEvidenceKind::MappedHeaderTokens,
                      ProofObligationKind::IncludeAnchorWitnessTracked,
                      ProofFailureReason::MissingIncludeAnchorWitness);
    discharge.Require(witness && witness->hasFirstPP && witness->hasLastPP &&
                          witness->firstPP <= witness->lastPP,
                      ProofObligationKind::IncludeMappedHeaderRangeTracked,
                      ProofFailureReason::MissingMappedHeaderRange);
    discharge.Require(witness && witness->hasByteRange &&
                          witness->startByte <= witness->endByte,
                      ProofObligationKind::IncludeMappedHeaderByteRangeTracked,
                      ProofFailureReason::MissingMappedHeaderByteRange);
    break;
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
    discharge.Require(patch->aStart == patch->aEnd,
                      ProofObligationKind::IncludePatchShapeTracked,
                      ProofFailureReason::MissingIncludePatchShape);
    discharge.Require(witness &&
                          witness->evidence ==
                              IncludeAnchorEvidenceKind::SelectedConditionalBoundary,
                      ProofObligationKind::IncludeSelectedConditionalBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeSelectedConditionalBoundaryWitness);
    discharge.Require(witness && witness->hasAnchorByte,
                      ProofObligationKind::IncludeAnchorByteTracked,
                      ProofFailureReason::MissingIncludeAnchorByte);
    discharge.Require(witness && witness->hasCondArmId,
                      ProofObligationKind::IncludeSelectedConditionalBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeSelectedConditionalBoundaryWitness);
    break;
  case AcceptedPathKind::IncludeInsertChildBoundary:
    discharge.Require(patch->aStart == patch->aEnd,
                      ProofObligationKind::IncludePatchShapeTracked,
                      ProofFailureReason::MissingIncludePatchShape);
    discharge.Require(witness &&
                          witness->evidence ==
                              IncludeAnchorEvidenceKind::ChildBoundary,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);
    discharge.Require(witness && witness->hasAnchorByte,
                      ProofObligationKind::IncludeAnchorByteTracked,
                      ProofFailureReason::MissingIncludeAnchorByte);
    discharge.Require(witness && witness->hasChildIncludeId,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);
    break;
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
    discharge.Require(patch->aStart == patch->aEnd,
                      ProofObligationKind::IncludePatchShapeTracked,
                      ProofFailureReason::MissingIncludePatchShape);
    discharge.Require(witness &&
                          witness->evidence ==
                              IncludeAnchorEvidenceKind::RightNeighborPP,
                      ProofObligationKind::IncludeRightNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeRightNeighborWitness);
    discharge.Require(witness && witness->hasAnchorByte,
                      ProofObligationKind::IncludeAnchorByteTracked,
                      ProofFailureReason::MissingIncludeAnchorByte);
    discharge.Require(witness && witness->hasNeighborPP,
                      ProofObligationKind::IncludeRightNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeRightNeighborWitness);
    break;
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
    discharge.Require(patch->aStart == patch->aEnd,
                      ProofObligationKind::IncludePatchShapeTracked,
                      ProofFailureReason::MissingIncludePatchShape);
    discharge.Require(witness &&
                          witness->evidence ==
                              IncludeAnchorEvidenceKind::LeftNeighborPP,
                      ProofObligationKind::IncludeLeftNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeLeftNeighborWitness);
    discharge.Require(witness && witness->hasAnchorByte,
                      ProofObligationKind::IncludeAnchorByteTracked,
                      ProofFailureReason::MissingIncludeAnchorByte);
    discharge.Require(witness && witness->hasNeighborPP,
                      ProofObligationKind::IncludeLeftNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeLeftNeighborWitness);
    break;
  case AcceptedPathKind::IncludeInsertDeclBoundary:
    discharge.Require(patch->aStart == patch->aEnd,
                      ProofObligationKind::IncludePatchShapeTracked,
                      ProofFailureReason::MissingIncludePatchShape);
    discharge.Require(witness &&
                          witness->evidence ==
                              IncludeAnchorEvidenceKind::DeclBoundary,
                      ProofObligationKind::IncludeDeclBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeDeclBoundaryWitness);
    discharge.Require(witness && witness->hasAnchorByte,
                      ProofObligationKind::IncludeAnchorByteTracked,
                      ProofFailureReason::MissingIncludeAnchorByte);
    discharge.Require(witness && witness->hasDeclHeaderRange &&
                          witness->anchorByte == witness->declHeaderE,
                      ProofObligationKind::IncludeDeclBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeDeclBoundaryWitness);
    break;
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    break;
  }

  return discharge.Finish();
}

bool RefoldEngine::IsAcceptedIncludeRealizationEvidenceKind(
    IncludeRealizationEvidenceKind kind) const {
  switch (kind) {
  case IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope:
  case IncludeRealizationEvidenceKind::ConsensusRescuedBCoverEnvelope:
    return true;
  case IncludeRealizationEvidenceKind::Unknown:
    return false;
  }
  return false;
}

RefoldEngine::ProofDischargeRecord
RefoldEngine::ValidateIncludeRealizationProof(
    AcceptedPathKind currentPath, const IncludePatch *patch,
    const IncludeRealizationWitness *witness) const {
  (void)patch;
  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory =
      BuildAcceptancePathInventory(currentPath);
  discharge.Require(inventory.currentPath != AcceptedPathKind::Unknown,
                    ProofObligationKind::AcceptedPathClassified,
                    ProofFailureReason::MissingAcceptedPathClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);
  discharge.Require(currentPath == AcceptedPathKind::IncludeRealizationInlineFromB ||
                        currentPath == AcceptedPathKind::IncludeMaterializedExpansion,
                    ProofObligationKind::AcceptedPathClassified,
                    ProofFailureReason::MissingAcceptedPathClassification);
  if (currentPath == AcceptedPathKind::IncludeRealizationInlineFromB) {
    // Step 4A formalizes the class predicate directly in the local proof:
    // inline include realization is discharged only when the witness records
    // either the canonical B-envelope mapping or the deterministic
    // consensus-rescue envelope. Any other evidence kind is outside the
    // declared domain for this proof class.
    discharge.Require(witness &&
                          IsAcceptedIncludeRealizationEvidenceKind(
                              witness->evidence),
                      ProofObligationKind::IncludeRealizationWitnessTracked,
                      ProofFailureReason::MissingIncludeRealizationWitness);
    discharge.Require(witness && witness->hasIncludeId && witness->includeId != 0,
                      ProofObligationKind::IncludeRealizationIncludeTracked,
                      ProofFailureReason::MissingIncludeRealizationInclude);
    discharge.Require(witness && witness->hasACover &&
                          witness->aCoverEnd >= witness->aCoverBegin,
                      ProofObligationKind::IncludeRealizationCoverTracked,
                      ProofFailureReason::MissingIncludeRealizationCover);
    discharge.Require(witness && witness->hasBTokenEnvelope &&
                          witness->bTokEnd >= witness->bTokBegin,
                      ProofObligationKind::IncludeRealizationBEnvelopeTracked,
                      ProofFailureReason::MissingIncludeRealizationBEnvelope);
  } else {
    discharge.Require(true, ProofObligationKind::AcceptedPathClassified,
                      ProofFailureReason::MissingAcceptedPathClassification);
  }
  return discharge.Finish();
}

RefoldEngine::ProofDischargeRecord
RefoldEngine::ValidateTUAnchorProof(AcceptedPathKind currentPath,
                                   const TUAnchorWitness *witness) const {
  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory =
      BuildAcceptancePathInventory(currentPath);
  const bool classified = currentPath == AcceptedPathKind::TUExactSlotBoundary ||
                          currentPath == AcceptedPathKind::TUProvableInsertionAnchor;
  discharge.Require(classified,
                    ProofObligationKind::TUAnchorPathClassified,
                    ProofFailureReason::MissingTUAnchorClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);
  // Step 7 requires every accepted TU anchor path to carry an explicit local
  // witness describing which deterministic anchor source succeeded.
  discharge.Require(witness && witness->evidence != TUAnchorEvidenceKind::Unknown,
                    ProofObligationKind::TUAnchorWitnessTracked,
                    ProofFailureReason::MissingTUAnchorWitness);
  discharge.Require(witness && witness->hasPPGap,
                    ProofObligationKind::TUAnchorPPGapTracked,
                    ProofFailureReason::MissingTUAnchorGap);
  discharge.Require(witness && witness->hasTUByte,
                    ProofObligationKind::TUAnchorByteTracked,
                    ProofFailureReason::MissingTUAnchorByte);

  if (!witness)
    return discharge.Finish();

  switch (currentPath) {
  case AcceptedPathKind::TUExactSlotBoundary: {
    const bool exactSlotWitness =
        witness->evidence == TUAnchorEvidenceKind::ExactSlotBoundary &&
        witness->exactPPMatch && witness->slotId != 0 &&
        !witness->slotKind.empty();
    discharge.Require(exactSlotWitness,
                      ProofObligationKind::TUExactSlotWitnessTracked,
                      ProofFailureReason::MissingTUExactSlotWitness);
    break;
  }

  case AcceptedPathKind::TUProvableInsertionAnchor: {
    bool provableWitness = false;
    switch (witness->evidence) {
    case TUAnchorEvidenceKind::ArgLikeBegin:
      provableWitness = (witness->macroId != 0);
      break;
    case TUAnchorEvidenceKind::ImmediateRightNeighbor:
      provableWitness = witness->hasRightNeighbor;
      break;
    case TUAnchorEvidenceKind::ImmediateLeftNeighbor:
      provableWitness = witness->hasLeftNeighbor;
      break;
    case TUAnchorEvidenceKind::CorroboratedRightNeighbor:
    case TUAnchorEvidenceKind::CorroboratedLeftNeighbor:
      provableWitness = witness->hasLeftNeighbor && witness->hasRightNeighbor;
      break;
    case TUAnchorEvidenceKind::Unknown:
    case TUAnchorEvidenceKind::ExactSlotBoundary:
      provableWitness = false;
      break;
    }
    discharge.Require(provableWitness,
                      ProofObligationKind::TUProvableEvidenceTracked,
                      ProofFailureReason::MissingTUProvableAnchorWitness);
    discharge.Require(witness->outsideIncludeCoverage,
                      ProofObligationKind::TUOutsideIncludeCoverageTracked,
                      ProofFailureReason::MissingTUOutsideIncludeCoverageProof);
    if (witness->evidence == TUAnchorEvidenceKind::CorroboratedRightNeighbor ||
        witness->evidence == TUAnchorEvidenceKind::CorroboratedLeftNeighbor) {
      discharge.Require(witness->ownerDepthStable,
                        ProofObligationKind::TUOwnerDepthStableTracked,
                        ProofFailureReason::MissingTUOwnerDepthStability);
    }
    break;
  }

  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    break;
  }
  return discharge.Finish();
}

StringRef
RefoldEngine::FormatAcceptedProofClass(AcceptedProofClass kind) const {
  switch (kind) {
  case AcceptedProofClass::Unknown:
    return "Unknown";
  case AcceptedProofClass::InvocationPreserving:
    return "InvocationPreserving";
  case AcceptedProofClass::InvocationRealization:
    return "InvocationRealization";
  case AcceptedProofClass::IncludePreserving:
    return "IncludePreserving";
  case AcceptedProofClass::IncludeRealization:
    return "IncludeRealization";
  case AcceptedProofClass::TUAnchor:
    return "TUAnchor";
  case AcceptedProofClass::TUTextualEdit:
    return "TUTextualEdit";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatAcceptedResultCandidateKind(
    AcceptedResultCandidateKind kind) const {
  switch (kind) {
  case AcceptedResultCandidateKind::Unknown:
    return "Unknown";
  case AcceptedResultCandidateKind::MacroPatch:
    return "MacroPatch";
  case AcceptedResultCandidateKind::IncludePatch:
    return "IncludePatch";
  case AcceptedResultCandidateKind::TUAnchor:
    return "TUAnchor";
  case AcceptedResultCandidateKind::TUTextEdit:
    return "TUTextEdit";
  case AcceptedResultCandidateKind::TerminalOutOfDomain:
    return "TerminalOutOfDomain";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatRealizationMode(RealizationMode mode) const {
  switch (mode) {
  case RealizationMode::Unknown:
    return "Unknown";
  case RealizationMode::PreserveOriginalStructure:
    return "PreserveOriginalStructure";
  case RealizationMode::RealizeEditedSurface:
    return "RealizeEditedSurface";
  }
  return "Unknown";
}

StringRef
RefoldEngine::FormatSelectionPreference(SelectionPreference preference) const {
  switch (preference) {
  case SelectionPreference::Unknown:
    return "Unknown";
  case SelectionPreference::PreferStructurePreservation:
    return "PreferStructurePreservation";
  case SelectionPreference::PreferSurfaceRealization:
    return "PreferSurfaceRealization";
  case SelectionPreference::PreferExactAnchoring:
    return "PreferExactAnchoring";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatSurfaceDisposition(
    SurfaceDisposition disposition) const {
  switch (disposition) {
  case SurfaceDisposition::None:
    return "None";
  case SurfaceDisposition::RealizeWholeCoverMacros:
    return "RealizeWholeCoverMacros";
  case SurfaceDisposition::RealizeInlineTouchedIncludesFromB:
    return "RealizeInlineTouchedIncludesFromB";
  case SurfaceDisposition::RealizeMaterializedIncludeExpansion:
    return "RealizeMaterializedIncludeExpansion";
  case SurfaceDisposition::RealizeTranslationUnitByteEdit:
    return "RealizeTranslationUnitByteEdit";
  case SurfaceDisposition::EmitEditedPreprocessedStream:
    return "EmitEditedPreprocessedStream";
  }
  return "None";
}

StringRef RefoldEngine::FormatAcceptedPathKind(AcceptedPathKind kind) const {
  switch (kind) {
  case AcceptedPathKind::Unknown:
    return "Unknown";
  case AcceptedPathKind::MacroArgsOnlyStandard:
    return "MacroArgsOnlyStandard";
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
    return "MacroArgsOnlyPasteSingle";
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
    return "MacroArgsOnlyPasteMulti";
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
    return "MacroArgsOnlyPurePasteOnly";
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
    return "MacroArgsOnlyPairedPureInsertion";
  case AcceptedPathKind::MacroDagSubtreeRoot:
    return "MacroDagSubtreeRoot";
  case AcceptedPathKind::MacroCallChainSuffix:
    return "MacroCallChainSuffix";
  case AcceptedPathKind::MacroCounterLiteral:
    return "MacroCounterLiteral";
  case AcceptedPathKind::MacroWholeCoverRealization:
    return "MacroWholeCoverRealization";
  case AcceptedPathKind::IncludePatchPendingMaterialization:
    return "IncludePatchPendingMaterialization";
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
    return "IncludeDeleteReplaceMappedHeaderTokens";
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
    return "IncludeInsertSelectedConditionalBoundary";
  case AcceptedPathKind::IncludeInsertChildBoundary:
    return "IncludeInsertChildBoundary";
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
    return "IncludeInsertRightNeighborPP";
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
    return "IncludeInsertLeftNeighborPP";
  case AcceptedPathKind::IncludeInsertDeclBoundary:
    return "IncludeInsertDeclBoundary";
  case AcceptedPathKind::IncludeRealizationInlineFromB:
    return "IncludeRealizationInlineFromB";
  case AcceptedPathKind::IncludeMaterializedExpansion:
    return "IncludeMaterializedExpansion";
  case AcceptedPathKind::TUExactSlotBoundary:
    return "TUExactSlotBoundary";
  case AcceptedPathKind::TUProvableInsertionAnchor:
    return "TUProvableInsertionAnchor";
  case AcceptedPathKind::TUByteSpanMappedEdit:
    return "TUByteSpanMappedEdit";
  case AcceptedPathKind::TUByteSpanConservativeEdit:
    return "TUByteSpanConservativeEdit";
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    return "TerminalEmitEditedPreprocessedStream";
  }
  return "Unknown";
}

StringRef
RefoldEngine::FormatAcceptanceSupportKind(AcceptanceSupportKind support) const {
  switch (support) {
  case AcceptanceSupportKind::Unknown:
    return "Unknown";
  case AcceptanceSupportKind::ExplicitProofBacked:
    return "ExplicitProofBacked";
  case AcceptanceSupportKind::DeterministicButNotFirstClass:
    return "DeterministicButNotFirstClass";
  case AcceptanceSupportKind::ExplicitOutOfDomainClass:
    return "ExplicitOutOfDomainClass";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatFutureProofTarget(FutureProofTarget target) const {
  switch (target) {
  case FutureProofTarget::Unknown:
    return "Unknown";
  case FutureProofTarget::MacroStandardArgsOnly:
    return "MacroStandardArgsOnly";
  case FutureProofTarget::MacroPasteSingle:
    return "MacroPasteSingle";
  case FutureProofTarget::MacroPasteMultiFixedAnchor:
    return "MacroPasteMultiFixedAnchor";
  case FutureProofTarget::MacroPurePasteOnly:
    return "MacroPurePasteOnly";
  case FutureProofTarget::MacroPairedPureInsertion:
    return "MacroPairedPureInsertion";
  case FutureProofTarget::MacroDagLift:
    return "MacroDagLift";
  case FutureProofTarget::MacroCallChainSuffixPreservation:
    return "MacroCallChainSuffixPreservation";
  case FutureProofTarget::MacroCounterStabilizationRealization:
    return "MacroCounterStabilizationRealization";
  case FutureProofTarget::MacroRealizationWholeCover:
    return "MacroRealizationWholeCover";
  case FutureProofTarget::IncludePatchByMappedHeaderTokens:
    return "IncludePatchByMappedHeaderTokens";
  case FutureProofTarget::IncludeConditionalArmCertifiedInsertion:
    return "IncludeConditionalArmCertifiedInsertion";
  case FutureProofTarget::IncludeInsertionByChildBoundary:
    return "IncludeInsertionByChildBoundary";
  case FutureProofTarget::IncludeInsertionByRightNeighborPP:
    return "IncludeInsertionByRightNeighborPP";
  case FutureProofTarget::IncludeInsertionByLeftNeighborPP:
    return "IncludeInsertionByLeftNeighborPP";
  case FutureProofTarget::IncludeInsertionByDeclBoundary:
    return "IncludeInsertionByDeclBoundary";
  case FutureProofTarget::IncludeRealizationCover:
    return "IncludeRealizationCover";
  case FutureProofTarget::IncludeMaterializedExpansionRealization:
    return "IncludeMaterializedExpansionRealization";
  case FutureProofTarget::TUExactSlotAnchor:
    return "TUExactSlotAnchor";
  case FutureProofTarget::TUProvableInsertionAnchor:
    return "TUProvableInsertionAnchor";
  case FutureProofTarget::TUByteSpanTextualEdit:
    return "TUByteSpanTextualEdit";
  case FutureProofTarget::EditedPreprocessedStreamFallback:
    return "ExplicitOutOfDomainTerminalResult";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatCompletenessCoverageKind(
    CompletenessCoverageKind kind) const {
  switch (kind) {
  case CompletenessCoverageKind::Unknown:
    return "Unknown";
  case CompletenessCoverageKind::DeclaredProofClass:
    return "DeclaredProofClass";
  case CompletenessCoverageKind::TransitionalGap:
    return "TransitionalGap";
  case CompletenessCoverageKind::ExplicitOutOfDomainClass:
    return "ExplicitOutOfDomainClass";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatCompletenessExpectationKind(
    CompletenessExpectationKind kind) const {
  switch (kind) {
  case CompletenessExpectationKind::Unknown:
    return "Unknown";
  case CompletenessExpectationKind::MustDiscoverDeclaredOrStrongerCompatible:
    return "MustDiscoverDeclaredOrStrongerCompatible";
  case CompletenessExpectationKind::NoClaimPendingClassClosure:
    return "NoClaimPendingClassClosure";
  case CompletenessExpectationKind::ExplicitlyOutsideDeclaredSet:
    return "ExplicitlyOutsideDeclaredSet";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatTheoremDomainKind(
    TheoremDomainKind kind) const {
  switch (kind) {
  case TheoremDomainKind::Unknown:
    return "Unknown";
  case TheoremDomainKind::DeclaredInDomainClass:
    return "DeclaredInDomainClass";
  case TheoremDomainKind::TransitionalGap:
    return "TransitionalGap";
  case TheoremDomainKind::ExplicitOutOfDomainClass:
    return "ExplicitOutOfDomainClass";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatLatticeConflictDomain(
    LatticeConflictDomain domain) const {
  switch (domain) {
  case LatticeConflictDomain::Unknown:
    return "Unknown";
  case LatticeConflictDomain::MacroInvocationRootSpan:
    return "MacroInvocationRootSpan";
  case LatticeConflictDomain::IncludeOwnerRegion:
    return "IncludeOwnerRegion";
  case LatticeConflictDomain::TUAnchorPoint:
    return "TUAnchorPoint";
  case LatticeConflictDomain::WholeTranslationUnit:
    return "WholeTranslationUnit";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatLatticeMergeLaw(LatticeMergeLaw law) const {
  switch (law) {
  case LatticeMergeLaw::Unknown:
    return "Unknown";
  case LatticeMergeLaw::DisjointCompose:
    return "DisjointCompose";
  case LatticeMergeLaw::NestedOuterShadowsInner:
    return "NestedOuterShadowsInner";
  case LatticeMergeLaw::SelectSingleWitness:
    return "SelectSingleWitness";
  case LatticeMergeLaw::TerminalReplacesAll:
    return "TerminalReplacesAll";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatLatticeConflictLaw(LatticeConflictLaw law) const {
  switch (law) {
  case LatticeConflictLaw::Unknown:
    return "Unknown";
  case LatticeConflictLaw::RejectPartialOverlap:
    return "RejectPartialOverlap";
  case LatticeConflictLaw::PreferStructurePreservation:
    return "PreferStructurePreservation";
  case LatticeConflictLaw::PreferExactAnchorWitness:
    return "PreferExactAnchorWitness";
  case LatticeConflictLaw::PreferOwnerPreservingBeforeRealization:
    return "PreferOwnerPreservingBeforeRealization";
  case LatticeConflictLaw::ExplicitOutOfDomainTerminalResult:
    return "ExplicitOutOfDomainTerminalResult";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatProofDischargeStatus(
    ProofDischargeStatus status) const {
  switch (status) {
  case ProofDischargeStatus::Unknown:
    return "Unknown";
  case ProofDischargeStatus::PendingMaterialization:
    return "PendingMaterialization";
  case ProofDischargeStatus::Discharged:
    return "Discharged";
  case ProofDischargeStatus::Rejected:
    return "Rejected";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatProofObligationKind(
    ProofObligationKind obligation) const {
  switch (obligation) {
  case ProofObligationKind::Unknown:
    return "Unknown";
  case ProofObligationKind::AcceptedPathClassified:
    return "AcceptedPathClassified";
  case ProofObligationKind::FutureTargetMapped:
    return "FutureTargetMapped";
  case ProofObligationKind::LegacyValidationRecorded:
    return "LegacyValidationRecorded";
  case ProofObligationKind::StructureMatchesAcceptedClass:
    return "StructureMatchesAcceptedClass";
  case ProofObligationKind::ProofRootTracked:
    return "ProofRootTracked";
  case ProofObligationKind::MacroProofRootResolved:
    return "MacroProofRootResolved";
  case ProofObligationKind::MacroProofRootIsTopLevel:
    return "MacroProofRootIsTopLevel";
  case ProofObligationKind::MacroPasteWitnessPresent:
    return "MacroPasteWitnessPresent";
  case ProofObligationKind::MacroPasteWitnessWellFormed:
    return "MacroPasteWitnessWellFormed";
  case ProofObligationKind::MacroPasteFreeSurfaceTracked:
    return "MacroPasteFreeSurfaceTracked";
  case ProofObligationKind::MacroSubtreeCertificateTracked:
    return "MacroSubtreeCertificateTracked";
  case ProofObligationKind::MacroCallChainWitnessTracked:
    return "MacroCallChainWitnessTracked";
  case ProofObligationKind::SubtreeAdmissibilityTracked:
    return "SubtreeAdmissibilityTracked";
  case ProofObligationKind::WholeCoverBoundsTracked:
    return "WholeCoverBoundsTracked";
  case ProofObligationKind::WholeCoverContainmentTracked:
    return "WholeCoverContainmentTracked";
  case ProofObligationKind::WholeCoverBoundaryAccountingTracked:
    return "WholeCoverBoundaryAccountingTracked";
  case ProofObligationKind::IncludePendingMaterializationClassified:
    return "IncludePendingMaterializationClassified";
  case ProofObligationKind::IncludePatchShapeTracked:
    return "IncludePatchShapeTracked";
  case ProofObligationKind::IncludeAnchorWitnessTracked:
    return "IncludeAnchorWitnessTracked";
  case ProofObligationKind::IncludeAnchorByteTracked:
    return "IncludeAnchorByteTracked";
  case ProofObligationKind::IncludeConditionalOwnershipTracked:
    return "IncludeConditionalOwnershipTracked";
  case ProofObligationKind::IncludeMappedHeaderRangeTracked:
    return "IncludeMappedHeaderRangeTracked";
  case ProofObligationKind::IncludeMappedHeaderByteRangeTracked:
    return "IncludeMappedHeaderByteRangeTracked";
  case ProofObligationKind::IncludeSelectedConditionalBoundaryWitnessTracked:
    return "IncludeSelectedConditionalBoundaryWitnessTracked";
  case ProofObligationKind::IncludeChildBoundaryWitnessTracked:
    return "IncludeChildBoundaryWitnessTracked";
  case ProofObligationKind::IncludeRightNeighborWitnessTracked:
    return "IncludeRightNeighborWitnessTracked";
  case ProofObligationKind::IncludeLeftNeighborWitnessTracked:
    return "IncludeLeftNeighborWitnessTracked";
  case ProofObligationKind::IncludeDeclBoundaryWitnessTracked:
    return "IncludeDeclBoundaryWitnessTracked";
  case ProofObligationKind::IncludeRealizationWitnessTracked:
    return "IncludeRealizationWitnessTracked";
  case ProofObligationKind::IncludeRealizationIncludeTracked:
    return "IncludeRealizationIncludeTracked";
  case ProofObligationKind::IncludeRealizationCoverTracked:
    return "IncludeRealizationCoverTracked";
  case ProofObligationKind::IncludeRealizationBEnvelopeTracked:
    return "IncludeRealizationBEnvelopeTracked";
  case ProofObligationKind::TUAnchorPathClassified:
    return "TUAnchorPathClassified";
  case ProofObligationKind::TUAnchorWitnessTracked:
    return "TUAnchorWitnessTracked";
  case ProofObligationKind::TUAnchorPPGapTracked:
    return "TUAnchorPPGapTracked";
  case ProofObligationKind::TUAnchorByteTracked:
    return "TUAnchorByteTracked";
  case ProofObligationKind::TUExactSlotWitnessTracked:
    return "TUExactSlotWitnessTracked";
  case ProofObligationKind::TUProvableEvidenceTracked:
    return "TUProvableEvidenceTracked";
  case ProofObligationKind::TUOutsideIncludeCoverageTracked:
    return "TUOutsideIncludeCoverageTracked";
  case ProofObligationKind::TUOwnerDepthStableTracked:
    return "TUOwnerDepthStableTracked";
  case ProofObligationKind::ExplicitOutOfDomainResultTracked:
    return "ExplicitOutOfDomainResultTracked";
  }
  return "Unknown";
}

StringRef RefoldEngine::FormatProofFailureReason(ProofFailureReason reason) const {
  switch (reason) {
  case ProofFailureReason::None:
    return "None";
  case ProofFailureReason::PendingMaterialization:
    return "PendingMaterialization";
  case ProofFailureReason::MissingAcceptedPathClassification:
    return "MissingAcceptedPathClassification";
  case ProofFailureReason::MissingFutureTargetMapping:
    return "MissingFutureTargetMapping";
  case ProofFailureReason::MissingLegacyValidation:
    return "MissingLegacyValidation";
  case ProofFailureReason::StructuralMismatch:
    return "StructuralMismatch";
  case ProofFailureReason::MissingProofRoot:
    return "MissingProofRoot";
  case ProofFailureReason::MissingMacroProofRootResolution:
    return "MissingMacroProofRootResolution";
  case ProofFailureReason::NonTopLevelMacroProofRoot:
    return "NonTopLevelMacroProofRoot";
  case ProofFailureReason::MissingPasteWitness:
    return "MissingPasteWitness";
  case ProofFailureReason::MalformedPasteWitness:
    return "MalformedPasteWitness";
  case ProofFailureReason::UnexpectedPasteSurface:
    return "UnexpectedPasteSurface";
  case ProofFailureReason::MissingSubtreeCertificate:
    return "MissingSubtreeCertificate";
  case ProofFailureReason::MissingCallChainWitness:
    return "MissingCallChainWitness";
  case ProofFailureReason::MissingSubtreeAdmissibility:
    return "MissingSubtreeAdmissibility";
  case ProofFailureReason::MissingWholeCoverBounds:
    return "MissingWholeCoverBounds";
  case ProofFailureReason::MissingWholeCoverContainment:
    return "MissingWholeCoverContainment";
  case ProofFailureReason::MissingWholeCoverBoundaryAccounting:
    return "MissingWholeCoverBoundaryAccounting";
  case ProofFailureReason::MissingIncludePatchShape:
    return "MissingIncludePatchShape";
  case ProofFailureReason::MissingIncludeAnchorWitness:
    return "MissingIncludeAnchorWitness";
  case ProofFailureReason::MissingIncludeAnchorByte:
    return "MissingIncludeAnchorByte";
  case ProofFailureReason::MissingConditionalOwnership:
    return "MissingConditionalOwnership";
  case ProofFailureReason::MissingMappedHeaderRange:
    return "MissingMappedHeaderRange";
  case ProofFailureReason::MissingMappedHeaderByteRange:
    return "MissingMappedHeaderByteRange";
  case ProofFailureReason::MissingIncludeSelectedConditionalBoundaryWitness:
    return "MissingIncludeSelectedConditionalBoundaryWitness";
  case ProofFailureReason::MissingIncludeChildBoundaryWitness:
    return "MissingIncludeChildBoundaryWitness";
  case ProofFailureReason::MissingIncludeRightNeighborWitness:
    return "MissingIncludeRightNeighborWitness";
  case ProofFailureReason::MissingIncludeLeftNeighborWitness:
    return "MissingIncludeLeftNeighborWitness";
  case ProofFailureReason::MissingIncludeDeclBoundaryWitness:
    return "MissingIncludeDeclBoundaryWitness";
  case ProofFailureReason::MissingIncludeRealizationWitness:
    return "MissingIncludeRealizationWitness";
  case ProofFailureReason::MissingIncludeRealizationInclude:
    return "MissingIncludeRealizationInclude";
  case ProofFailureReason::MissingIncludeRealizationCover:
    return "MissingIncludeRealizationCover";
  case ProofFailureReason::MissingIncludeRealizationBEnvelope:
    return "MissingIncludeRealizationBEnvelope";
  case ProofFailureReason::MissingTUAnchorClassification:
    return "MissingTUAnchorClassification";
  case ProofFailureReason::MissingTUAnchorWitness:
    return "MissingTUAnchorWitness";
  case ProofFailureReason::MissingTUAnchorGap:
    return "MissingTUAnchorGap";
  case ProofFailureReason::MissingTUAnchorByte:
    return "MissingTUAnchorByte";
  case ProofFailureReason::MissingTUExactSlotWitness:
    return "MissingTUExactSlotWitness";
  case ProofFailureReason::MissingTUProvableAnchorWitness:
    return "MissingTUProvableAnchorWitness";
  case ProofFailureReason::MissingTUOutsideIncludeCoverageProof:
    return "MissingTUOutsideIncludeCoverageProof";
  case ProofFailureReason::MissingTUOwnerDepthStability:
    return "MissingTUOwnerDepthStability";
  case ProofFailureReason::ExplicitOutOfDomainResult:
    return "ExplicitOutOfDomainResult";
  }
  return "None";
}

StringRef RefoldEngine::FormatTUAnchorEvidenceKind(
    TUAnchorEvidenceKind kind) const {
  switch (kind) {
  case TUAnchorEvidenceKind::Unknown:
    return "Unknown";
  case TUAnchorEvidenceKind::ExactSlotBoundary:
    return "ExactSlotBoundary";
  case TUAnchorEvidenceKind::ArgLikeBegin:
    return "ArgLikeBegin";
  case TUAnchorEvidenceKind::ImmediateRightNeighbor:
    return "ImmediateRightNeighbor";
  case TUAnchorEvidenceKind::ImmediateLeftNeighbor:
    return "ImmediateLeftNeighbor";
  case TUAnchorEvidenceKind::CorroboratedRightNeighbor:
    return "CorroboratedRightNeighbor";
  case TUAnchorEvidenceKind::CorroboratedLeftNeighbor:
    return "CorroboratedLeftNeighbor";
  }
  return "Unknown";
}

std::string RefoldEngine::FormatTUAnchorWitness(
    const TUAnchorWitness &witness) const {
  return formatv(
             "kind={0} ppGap={1}{2} tuByte={3}{4} slotId={5} slotKind={6} "
             "macroId={7} leftPP={8}{9} rightPP={10}{11} outsideInclude={12} "
             "ownerDepthStable={13} exactPP={14}",
             FormatTUAnchorEvidenceKind(witness.evidence), witness.ppGap,
             witness.hasPPGap ? "" : "(missing)", witness.tuByte,
             witness.hasTUByte ? "" : "(missing)", witness.slotId,
             witness.slotKind.empty() ? StringRef("(none)") : StringRef(witness.slotKind),
             witness.macroId, witness.leftNeighborPP,
             witness.hasLeftNeighbor ? "" : "(missing)", witness.rightNeighborPP,
             witness.hasRightNeighbor ? "" : "(missing)",
             witness.outsideIncludeCoverage ? 1 : 0,
             witness.ownerDepthStable ? 1 : 0, witness.exactPPMatch ? 1 : 0)
      .str();
}

StringRef RefoldEngine::FormatIncludeAnchorEvidenceKind(
    IncludeAnchorEvidenceKind kind) const {
  switch (kind) {
  case IncludeAnchorEvidenceKind::Unknown:
    return "Unknown";
  case IncludeAnchorEvidenceKind::MappedHeaderTokens:
    return "MappedHeaderTokens";
  case IncludeAnchorEvidenceKind::SelectedConditionalBoundary:
    return "SelectedConditionalBoundary";
  case IncludeAnchorEvidenceKind::ChildBoundary:
    return "ChildBoundary";
  case IncludeAnchorEvidenceKind::RightNeighborPP:
    return "RightNeighborPP";
  case IncludeAnchorEvidenceKind::LeftNeighborPP:
    return "LeftNeighborPP";
  case IncludeAnchorEvidenceKind::DeclBoundary:
    return "DeclBoundary";
  }
  return "Unknown";
}

std::string RefoldEngine::FormatIncludeAnchorWitness(
    const IncludeAnchorWitness &witness) const {
  return formatv(
             "kind={0} anchorByte={1}{2} byteRange=[{3},{4}){5} firstPP={6}{7} "
             "lastPP={8}{9} neighborPP={10}{11} condArm={12}{13} childInc={14}{15} "
             "decl=[{16},{17}){18}",
             FormatIncludeAnchorEvidenceKind(witness.evidence),
             witness.anchorByte, witness.hasAnchorByte ? "" : "(missing)",
             witness.startByte, witness.endByte,
             witness.hasByteRange ? "" : "(missing)", witness.firstPP,
             witness.hasFirstPP ? "" : "(missing)", witness.lastPP,
             witness.hasLastPP ? "" : "(missing)", witness.neighborPP,
             witness.hasNeighborPP ? "" : "(missing)", witness.condArmId,
             witness.hasCondArmId ? "" : "(missing)", witness.childIncludeId,
             witness.hasChildIncludeId ? "" : "(missing)", witness.declHeaderB,
             witness.declHeaderE, witness.hasDeclHeaderRange ? "" : "(missing)")
      .str();
}

StringRef RefoldEngine::FormatIncludeRealizationEvidenceKind(
    IncludeRealizationEvidenceKind kind) const {
  switch (kind) {
  case IncludeRealizationEvidenceKind::Unknown:
    return "Unknown";
  case IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope:
    return "CanonicalBCoverEnvelope";
  case IncludeRealizationEvidenceKind::ConsensusRescuedBCoverEnvelope:
    return "ConsensusRescuedBCoverEnvelope";
  }
  return "Unknown";
}

std::string RefoldEngine::FormatIncludeRealizationWitness(
    const IncludeRealizationWitness &witness) const {
  return formatv(
             "kind={0} includeId={1}{2} aCover=[{3},{4}){5} "
             "bTokEnvelope=[{6},{7}){8}",
             FormatIncludeRealizationEvidenceKind(witness.evidence),
             witness.includeId, witness.hasIncludeId ? "" : "(missing)",
             witness.aCoverBegin, witness.aCoverEnd,
             witness.hasACover ? "" : "(missing)", witness.bTokBegin,
             witness.bTokEnd,
             witness.hasBTokenEnvelope ? "" : "(missing)")
      .str();
}

RefoldEngine::TerminalFallbackWitness
RefoldEngine::BuildTerminalFallbackWitness() const {
  TerminalFallbackWitness witness;
  witness.kind = terminalFallbackKind_;
  witness.requestCount = terminalFallbackRequestCount_;
  if (!terminalFallbackReasons_.empty()) {
    witness.hasPrimaryReason = true;
    witness.primaryReason = terminalFallbackReasons_.front();
  }
  return witness;
}

bool RefoldEngine::IsOwnerUnresolvedNoTUAnchorOutOfDomain(
    const diffutils::Hunk &h, StringRef tuPath, const Owner &owner,
    bool mapsToTU) const {
  // The declared domain excludes this case only after all deterministic owner
  // and TU witness searches have already failed. This helper is intentionally
  // derived-only: it does not invent new anchors or infer ownership from
  // proximity.
  if (owner.kind != OwnerKind::Unknown)
    return false;

  if (h.isInsertOnly()) {
    if (BoundaryParentIncludeForPureInsertion(h))
      return false;
    if (mapsToTU)
      return false;
    if (FindProvableTUInsertionAnchor(h.aStart, tuPath))
      return false;
    if (TUByteSpan(h.aStart, h.aEnd, tuPath))
      return false;
    return true;
  }

  if (mapsToTU)
    return false;
  if (TUByteSpan(h.aStart, h.aEnd, tuPath))
    return false;
  return true;
}

std::string RefoldEngine::BuildOwnerUnresolvedNoTUAnchorDetail(
    size_t hunkIndex, const diffutils::Hunk &h, StringRef tuPath,
    const Owner &owner, bool mapsToTU) const {
  const bool isInsertion = h.aStart == h.aEnd;
  // Reaching this detail builder already means the earlier macro-owner and
  // include-owner searches failed to claim the edit. Record that control-flow
  // fact explicitly so the terminal witness states the declared domain wall in
  // theorem terms rather than only in terms of the final `Owner` enum.
  const bool macroOwnerExhausted = true;
  const bool includeOwnerExhausted = owner.kind != OwnerKind::Include;

  const RefoldModel::IncludeItem *boundaryInc = nullptr;
  if (isInsertion)
    boundaryInc = BoundaryParentIncludeForPureInsertion(h);
  const bool hasBoundaryInclude = boundaryInc != nullptr;

  std::optional<uint64_t> provableInsertionAnchor;
  if (isInsertion)
    provableInsertionAnchor = FindProvableTUInsertionAnchor(h.aStart, tuPath);
  const bool hasProvableInsertionAnchor = provableInsertionAnchor.has_value();

  std::optional<std::pair<uint64_t, uint64_t>> tuSpan =
      TUByteSpan(h.aStart, h.aEnd, tuPath);
  const bool hasTUByteSpan = tuSpan.has_value();
  const bool declaredDomainWall =
      IsOwnerUnresolvedNoTUAnchorOutOfDomain(h, tuPath, owner, mapsToTU);

  std::string exactSlot = "n/a";
  std::string insertionAnchor = "n/a";
  std::string boundaryParent = "n/a";
  if (isInsertion) {
    if (auto slot = AnchorToExactSlotBoundaryFromPPGap(tuPath, h.aStart))
      exactSlot = llvm::formatv("{0}", *slot).str();
    else
      exactSlot = "none";

    if (provableInsertionAnchor)
      insertionAnchor = llvm::formatv("{0}", *provableInsertionAnchor).str();
    else
      insertionAnchor = "none";

    if (boundaryInc)
      boundaryParent = llvm::formatv("inc#{0}", boundaryInc->id).str();
    else
      boundaryParent = "none";
  }

  const std::string tuSpanStr =
      tuSpan ? llvm::formatv("[{0},{1})", tuSpan->first, tuSpan->second).str()
             : std::string("none");

  return llvm::formatv(
             "edit #{0} A=[{1},{2}) insert={3} owner={4} macroOwnerExhausted={5} "
             "includeOwnerExhausted={6} mapsToTU={7} TUByteSpan={8} hasTUByteSpan={9} "
             "exactSlot={10} insertionAnchor={11} hasProvableInsertionAnchor={12} "
             "boundaryParentInclude={13} hasBoundaryInclude={14} declaredDomainWall={15}",
             hunkIndex, h.aStart, h.aEnd, isInsertion ? "yes" : "no",
             toString(owner.kind), macroOwnerExhausted ? "yes" : "no",
             includeOwnerExhausted ? "yes" : "no", mapsToTU ? "yes" : "no",
             tuSpanStr, hasTUByteSpan ? "yes" : "no", exactSlot,
             insertionAnchor, hasProvableInsertionAnchor ? "yes" : "no",
             boundaryParent, hasBoundaryInclude ? "yes" : "no",
             declaredDomainWall ? "yes" : "no")
      .str();
}

std::string RefoldEngine::FormatTerminalFallbackWitness(
    const TerminalFallbackWitness &witness) const {
  auto formatKind = [](TerminalFallbackKind kind) -> const char * {
    switch (kind) {
    case TerminalFallbackKind::OwnerUnresolvedNoTUAnchor:
      return "OwnerUnresolvedNoTUAnchor";
    case TerminalFallbackKind::IncludeRealizationUnmappableBCoverEnvelope:
      return "IncludeRealizationUnmappableBCoverEnvelope";
    case TerminalFallbackKind::UndischargedEmissionArtifact:
      return "UndischargedEmissionArtifact";
    case TerminalFallbackKind::TheoremAuditInvariantViolation:
      return "TheoremAuditInvariantViolation";
    case TerminalFallbackKind::MixedExcludedCases:
      return "MixedExcludedCases";
    case TerminalFallbackKind::Unknown:
      return "Unknown";
    }
    return "Unknown";
  };

  if (!witness.hasPrimaryReason)
    return formatv("kind={0} requestCount={1}", formatKind(witness.kind),
                   witness.requestCount)
        .str();

  return formatv("kind={0} requestCount={1} primaryReason='{2}'",
                 formatKind(witness.kind), witness.requestCount,
                 stringutils::showWSWithClip(witness.primaryReason, 200))
      .str();
}

StringRef
RefoldEngine::FormatMacroPatchProofKind(MacroPatchProofKind kind) const {
  switch (kind) {
  case MacroPatchProofKind::Unknown:
    return "Unknown";
  case MacroPatchProofKind::CounterLiteral:
    return "CounterLiteral";
  case MacroPatchProofKind::ArgsOnlyPasteMulti:
    return "ArgsOnlyPasteMulti";
  case MacroPatchProofKind::ArgsOnlyPasteSingle:
    return "ArgsOnlyPasteSingle";
  case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
    return "ArgsOnlyPurePasteOnly";
  case MacroPatchProofKind::ArgsOnlyStandard:
    return "ArgsOnlyStandard";
  case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    // Paired pure insertion is only valid on non-paste direct arg/stringify
    // surfaces. The builder already enforces that; Step 6 records it.
    return "ArgsOnlyPairedPureInsertion";
  case MacroPatchProofKind::DagSubtreeRoot:
    // DAG-preserving rewrites must carry the explicit subtree certificate that
    // Step 5 started recording on accepted root patches.
    return "DagSubtreeRoot";
  case MacroPatchProofKind::CallChainSuffix:
    // Call-chain suffix rewrites are emitted directly on the root callsite
    // slice, so the patch's owning macro id must already be that root.
    return "CallChainSuffix";
  case MacroPatchProofKind::WholeCoverRealization:
    return "WholeCoverRealization";
  }
  return "Unknown";
}

std::string RefoldEngine::FormatAcceptancePathInventory(
    const AcceptancePathInventory &inventory) const {
  return formatv("currentPath={0} support={1} futureTarget={2}",
                 FormatAcceptedPathKind(inventory.currentPath),
                 FormatAcceptanceSupportKind(inventory.support),
                 FormatFutureProofTarget(inventory.futureTarget))
      .str();
}

std::string RefoldEngine::FormatProofDischargeRecord(
    const ProofDischargeRecord &record) const {
  return formatv(
             "status={0} evaluated={1} satisfied={2} failedObligation={3} "
             "failureReason={4}",
             FormatProofDischargeStatus(record.status),
             record.obligationsEvaluated, record.obligationsSatisfied,
             FormatProofObligationKind(record.failedObligation),
             FormatProofFailureReason(record.failureReason))
      .str();
}

std::string RefoldEngine::FormatGlobalSelectionLattice(
    const GlobalSelectionLattice &lattice) const {
  return formatv("domain={0} mergeLaw={1} conflictLaw={2}",
                 FormatLatticeConflictDomain(lattice.domain),
                 FormatLatticeMergeLaw(lattice.mergeLaw),
                 FormatLatticeConflictLaw(lattice.conflictLaw))
      .str();
}

std::string RefoldEngine::FormatCompletenessContract(
    const CompletenessContract &contract) const {
  if (contract.hasExplicitExclusion) {
    const TerminalFallbackWitness witness{contract.explicitExclusion, 0, false, std::string()};
    return formatv("coverage={0} expectation={1} declaredTarget={2} counts={3} exclusion={4}",
                   FormatCompletenessCoverageKind(contract.coverage),
                   FormatCompletenessExpectationKind(contract.expectation),
                   FormatFutureProofTarget(contract.declaredTarget),
                   contract.countsTowardDeclaredCoverage ? 1 : 0,
                   FormatTerminalFallbackWitness(witness))
        .str();
  }
  return formatv(
             "coverage={0} expectation={1} declaredTarget={2} counts={3}",
             FormatCompletenessCoverageKind(contract.coverage),
             FormatCompletenessExpectationKind(contract.expectation),
             FormatFutureProofTarget(contract.declaredTarget),
             contract.countsTowardDeclaredCoverage ? 1 : 0)
      .str();
}

std::string RefoldEngine::FormatTheoremDomainContract(
    const TheoremDomainContract &contract) const {
  if (contract.hasExplicitExclusion) {
    const TerminalFallbackWitness witness{contract.explicitExclusion, 0, false, std::string()};
    return formatv(
               "kind={0} inDeclaredDomain={1} counts={2} declaredTarget={3} exclusion={4}",
               FormatTheoremDomainKind(contract.kind),
               contract.inDeclaredDomain ? 1 : 0,
               contract.countsTowardCompleteness ? 1 : 0,
               FormatFutureProofTarget(contract.declaredTarget),
               FormatTerminalFallbackWitness(witness))
        .str();
  }
  return formatv(
             "kind={0} inDeclaredDomain={1} counts={2} declaredTarget={3}",
             FormatTheoremDomainKind(contract.kind),
             contract.inDeclaredDomain ? 1 : 0,
             contract.countsTowardCompleteness ? 1 : 0,
             FormatFutureProofTarget(contract.declaredTarget))
      .str();
}

std::string RefoldEngine::FormatAcceptedPathAudit(
    AcceptedPathKind currentPath, const IncludePatch *patch,
    const TUAnchorWitness *tuAnchorWitness,
    const IncludeAnchorWitness *includeAnchorWitness,
    const IncludeRealizationWitness *includeRealizationWitness,
    const TerminalFallbackWitness *terminalFallbackWitness) const {
  const ProofSummary summary = BuildAcceptedPathProofSummary(
      currentPath, patch, tuAnchorWitness, includeAnchorWitness,
      includeRealizationWitness, terminalFallbackWitness);
  if (summary.hasTUAnchorWitness) {
    return formatv("inventory={0} lattice={1} completeness={2} theoremDomain={3} discharge={4} tuAnchor={5}",
                   FormatAcceptancePathInventory(summary.inventory),
                   FormatGlobalSelectionLattice(summary.lattice),
                   FormatCompletenessContract(summary.completeness),
                   FormatTheoremDomainContract(summary.theoremDomain),
                   FormatProofDischargeRecord(summary.discharge),
                   FormatTUAnchorWitness(summary.tuAnchorWitness))
        .str();
  }
  if (summary.hasIncludeAnchorWitness) {
    return formatv("inventory={0} lattice={1} completeness={2} theoremDomain={3} discharge={4} includeAnchor={5}",
                   FormatAcceptancePathInventory(summary.inventory),
                   FormatGlobalSelectionLattice(summary.lattice),
                   FormatCompletenessContract(summary.completeness),
                   FormatTheoremDomainContract(summary.theoremDomain),
                   FormatProofDischargeRecord(summary.discharge),
                   FormatIncludeAnchorWitness(summary.includeAnchorWitness))
        .str();
  }
  if (summary.hasIncludeRealizationWitness) {
    return formatv("inventory={0} lattice={1} completeness={2} theoremDomain={3} discharge={4} includeRealization={5}",
                   FormatAcceptancePathInventory(summary.inventory),
                   FormatGlobalSelectionLattice(summary.lattice),
                   FormatCompletenessContract(summary.completeness),
                   FormatTheoremDomainContract(summary.theoremDomain),
                   FormatProofDischargeRecord(summary.discharge),
                   FormatIncludeRealizationWitness(
                       summary.includeRealizationWitness))
        .str();
  }
  if (summary.hasTerminalFallbackWitness) {
    return formatv("inventory={0} lattice={1} completeness={2} theoremDomain={3} discharge={4} terminalOutOfDomain={5}",
                   FormatAcceptancePathInventory(summary.inventory),
                   FormatGlobalSelectionLattice(summary.lattice),
                   FormatCompletenessContract(summary.completeness),
                   FormatTheoremDomainContract(summary.theoremDomain),
                   FormatProofDischargeRecord(summary.discharge),
                   FormatTerminalFallbackWitness(summary.terminalFallbackWitness))
        .str();
  }
  return formatv("inventory={0} lattice={1} completeness={2} theoremDomain={3} discharge={4}",
                 FormatAcceptancePathInventory(summary.inventory),
                 FormatGlobalSelectionLattice(summary.lattice),
                 FormatCompletenessContract(summary.completeness),
                 FormatTheoremDomainContract(summary.theoremDomain),
                 FormatProofDischargeRecord(summary.discharge))
      .str();
}

std::string
RefoldEngine::FormatAcceptedResultCandidate(
    const AcceptedResultCandidate &candidate) const {
  std::string artifact;
  switch (candidate.kind) {
  case AcceptedResultCandidateKind::MacroPatch:
    artifact = formatv("span=[{0},{1})", candidate.begin, candidate.end).str();
    if (candidate.hasRootMacroId) {
      artifact += formatv(" rootMacro={0}", candidate.rootMacroId).str();
    }
    break;
  case AcceptedResultCandidateKind::IncludePatch:
    artifact = formatv("aSpan=[{0},{1})", candidate.begin, candidate.end).str();
    if (candidate.hasOwnerIncludeId) {
      artifact += formatv(" ownerInclude={0}", candidate.ownerIncludeId).str();
    }
    if (candidate.hasAnchorByte) {
      artifact += formatv(" anchorByte={0}", candidate.anchorByte).str();
    }
    break;
  case AcceptedResultCandidateKind::TUAnchor:
    artifact = formatv("ppGap={0}", candidate.begin).str();
    if (candidate.hasAnchorByte) {
      artifact += formatv(" tuByte={0}", candidate.anchorByte).str();
    }
    break;
  case AcceptedResultCandidateKind::TUTextEdit:
    artifact = formatv("bytes=[{0},{1})", candidate.begin, candidate.end).str();
    break;
  case AcceptedResultCandidateKind::TerminalOutOfDomain:
  case AcceptedResultCandidateKind::Unknown:
    break;
  }

  if (candidate.proofSummary.hasTUAnchorWitness) {
    artifact += formatv(" tuAnchor={0}",
                        FormatTUAnchorWitness(
                            candidate.proofSummary.tuAnchorWitness))
                    .str();
  }
  if (candidate.proofSummary.hasIncludeAnchorWitness) {
    artifact += formatv(" includeAnchor={0}",
                        FormatIncludeAnchorWitness(
                            candidate.proofSummary.includeAnchorWitness))
                    .str();
  }
  if (candidate.proofSummary.hasIncludeRealizationWitness) {
    artifact +=
        formatv(" includeRealization={0}",
                FormatIncludeRealizationWitness(
                    candidate.proofSummary.includeRealizationWitness))
            .str();
  }
  if (candidate.proofSummary.hasTerminalFallbackWitness) {
    artifact += formatv(" terminalOutOfDomain={0}",
                        FormatTerminalFallbackWitness(
                            candidate.proofSummary.terminalFallbackWitness))
                    .str();
  }
  if (candidate.hasPayloadPreview) {
    artifact += formatv(" payload='{0}'", candidate.payloadPreview).str();
  }

  const std::string artifactSuffix =
      artifact.empty() ? std::string() : formatv(" artifact={0}", artifact).str();
  return formatv(
             "kind={0} class={1} realization={2} preference={3} "
             "surfaceDisposition={4} inventory={5} lattice={6} "
             "completeness={7} theoremDomain={8} discharge={9}{10}",
             FormatAcceptedResultCandidateKind(candidate.kind),
             FormatAcceptedProofClass(candidate.proofSummary.acceptedClass),
             FormatRealizationMode(candidate.proofSummary.realizationMode),
             FormatSelectionPreference(candidate.proofSummary.preference),
             FormatSurfaceDisposition(candidate.proofSummary.surfaceDisposition),
             FormatAcceptancePathInventory(candidate.proofSummary.inventory),
             FormatGlobalSelectionLattice(candidate.proofSummary.lattice),
             FormatCompletenessContract(candidate.proofSummary.completeness),
             FormatTheoremDomainContract(candidate.proofSummary.theoremDomain),
             FormatProofDischargeRecord(candidate.proofSummary.discharge),
             artifactSuffix)
      .str();
}

std::string RefoldEngine::FormatMacroPatchAudit(const MacroPatch &patch) const {
  const ProofSummary summary = ClassifyMacroPatchProof(patch);
  return formatv(
             "proofKind={0} topClass={1} realization={2} preference={3} "
             "surfaceDisposition={4} inventory={5} lattice={6} completeness={7} "
             "theoremDomain={8} discharge={9} validated={10} struct={11} proofRoot={12} subtreeCert={13} "
             "leaf={14} witnesses={15} invCerts={16} formalCerts={17} "
             "argCerts={18} liftChains={19} liftSteps={20} rootMerges={21} "
             "lexicalBridge={22} paste={23} wrappers={24} stringify={25} "
             "wideStringify={26} childSyntax={27} rawInvocation={28} "
             "passthrough={29} bridgeSensitive={30} "
             "deferredPasteDischarged={31} admissible={32} "
             "pasteReplayValidated={33} expRootN={34} "
             "deferredRootN={35} bridgeFormalN={36} expRoot={37} "
             "deferredRootArgs={38} bridgeFormals={39} wholeCoverA=[{40},{41}) "
             "wholeCoverBraw=[{42},{43}) wholeCoverBadj=[{44},{45})",
             FormatMacroPatchProofKind(patch.proofKind),
             FormatAcceptedProofClass(summary.acceptedClass),
             FormatRealizationMode(summary.realizationMode),
             FormatSelectionPreference(summary.preference),
             FormatSurfaceDisposition(summary.surfaceDisposition),
             FormatAcceptancePathInventory(summary.inventory),
             FormatGlobalSelectionLattice(summary.lattice),
             FormatCompletenessContract(summary.completeness),
             FormatTheoremDomainContract(summary.theoremDomain),
             FormatProofDischargeRecord(summary.discharge),
             patch.proofValidated ? 1 : 0,
             patch.structurePreserving ? 1 : 0, patch.proofRootMacroId,
             patch.subtreeCertBacked ? 1 : 0, patch.subtreeLeafMacroId,
             patch.subtreeWitnessCount, patch.subtreeInvocationCertCount,
             patch.subtreeFormalCertCount, patch.subtreeArgCertCount,
             patch.subtreeLiftChainCount, patch.subtreeLiftStepCount,
             patch.subtreeRootMergeCount,
             patch.subtreeUsesLexicalBridge ? 1 : 0,
             patch.subtreeTouchesPaste ? 1 : 0,
             patch.subtreeHasWrapperSemantics ? 1 : 0,
             patch.subtreeHasStringifySemantics ? 1 : 0,
             patch.subtreeHasWideStringifySemantics ? 1 : 0,
             patch.subtreeHasPreferredChildSyntax ? 1 : 0,
             patch.subtreeHasRawInvocationPreservation ? 1 : 0,
             patch.subtreeHasPassthroughFlatten ? 1 : 0,
             patch.subtreeHasBridgeSensitiveStructuredSemantics ? 1 : 0,
             patch.subtreeDeferredPasteDischarged ? 1 : 0,
             patch.subtreeAdmissible ? 1 : 0,
             patch.pasteReplayValidated ? 1 : 0,
             patch.subtreeExpectedRootFormalCount,
             patch.subtreeDeferredRootArgCount,
             patch.subtreeBridgeSensitiveFormalCount,
             stringutils::showWSWithClip(patch.subtreeExpectedRootFormalSummary, 160),
             stringutils::showWSWithClip(patch.subtreeDeferredRootArgSummary, 160),
             stringutils::showWSWithClip(patch.subtreeBridgeSensitiveFormalSummary, 160),
             patch.wholeCoverALo, patch.wholeCoverAHi, patch.wholeCoverBRawLo,
             patch.wholeCoverBRawHi, patch.wholeCoverBAdjLo,
             patch.wholeCoverBAdjHi)
      .str();
}

std::optional<std::string>
RefoldEngine::BuildWholeCoverReplacementText(
    const RefoldModel::MacroInvocation &m) const {
  auto plan = ComputeWholeCoverPlan(m);
  if (!plan)
    return std::nullopt;
  return plan->clippedText;
}

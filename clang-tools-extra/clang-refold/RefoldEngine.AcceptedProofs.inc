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
  // Consume the exact `paste_tokens` witnesses serialized by the producer.
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
RefoldEngine::ValidateInvocationPreservingProofImpl(
    const MacroPatch &patch, bool requireTopLevelRoot) const {
  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory =
      InventoryMacroPatchAcceptancePath(patch);

  RequireAcceptedPathBaseline(discharge, inventory);
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
  if (requireTopLevelRoot && root) {
    discharge.Require(GetRootMacroId(root->id) == root->id,
                      ProofObligationKind::MacroProofRootIsTopLevel,
                      ProofFailureReason::NonTopLevelMacroProofRoot);
  }

  switch (patch.proofKind) {
  case MacroPatchProofKind::ArgsOnlyStandard:
    break;

  case MacroPatchProofKind::ArgsOnlyPasteSingle:
  case MacroPatchProofKind::ArgsOnlyPasteMulti:
  case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
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

  case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    discharge.Require(root && root->pasteSpans.empty(),
                      ProofObligationKind::MacroPasteFreeSurfaceTracked,
                      ProofFailureReason::UnexpectedPasteSurface);
    break;

  case MacroPatchProofKind::PasteDerivedCalleeSelector:
    // The selector-substitution builder validates the hard semantic witness
    // before stamping the patch: a single existing pasted-callee target must
    // reproduce the edited B expansion, the original target must reproduce the
    // A expansion under the same non-selector arguments, and the root edit must
    // rewrite only the proved selector argument. Keep validation here to the
    // common invocation-preserving obligations above so compatible same-root
    // merges do not become invalid merely because their materialized edit-map
    // subrange is no longer representable as one selector slice.
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
RefoldEngine::ValidateInvocationPreservingProof(const MacroPatch &patch) const {
  return ValidateInvocationPreservingProofImpl(
      patch, /*requireTopLevelRoot=*/true);
}

RefoldEngine::ProofDischargeRecord
RefoldEngine::ValidateEmittedInvocationPreservingProof(
    const MacroPatch &patch) const {
  return ValidateInvocationPreservingProofImpl(
      patch, /*requireTopLevelRoot=*/false);
}

RefoldEngine::ProofDischargeRecord
RefoldEngine::ValidateInvocationRealizationProof(
    const MacroPatch &patch) const {
  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory =
      InventoryMacroPatchAcceptancePath(patch);

  // Invocation-realization patches still need the common accepted-path
  // metadata, but unlike invocation-preserving proofs they are expected to
  // realize expansion text rather than preserve source invocation structure.
  RequireAcceptedPathBaseline(discharge, inventory);
  discharge.Require(patch.proofRootMacroId != 0,
                    ProofObligationKind::ProofRootTracked,
                    ProofFailureReason::MissingProofRoot);

  if (patch.proofKind == MacroPatchProofKind::WholeCoverRealization) {
    // Whole-cover realization has additional audit obligations: the A/B token
    // envelopes, adjusted B envelope, containment status, and boundary
    // accounting must all be explicitly recorded on the emitted patch.
    const bool boundsTracked =
        patch.wholeCoverALo <= patch.wholeCoverAHi &&
        patch.wholeCoverBRawLo <= patch.wholeCoverBRawHi &&
        patch.wholeCoverBAdjLo <= patch.wholeCoverBAdjHi;
    const bool boundaryAccountingTracked =
        boundsTracked && patch.wholeCoverBRawLo <= patch.wholeCoverBAdjLo &&
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
    // Other realization paths currently rely on the common legacy validation
    // bit plus the non-structure-preserving accepted class. They do not carry
    // the whole-cover-specific envelope metadata checked above.
    discharge.Require(patch.proofValidated,
                      ProofObligationKind::LegacyValidationRecorded,
                      ProofFailureReason::MissingLegacyValidation);
    discharge.Require(!patch.structurePreserving,
                      ProofObligationKind::StructureMatchesAcceptedClass,
                      ProofFailureReason::StructuralMismatch);
  }

  return discharge.Finish();
}

RefoldEngine::ProofDischargeRecord RefoldEngine::ValidateIncludePreservingProof(
    AcceptedPathKind currentPath, const IncludePatch *patch,
    const IncludeAnchorWitness *witness) const {
  if (currentPath == AcceptedPathKind::IncludePatchPendingMaterialization) {
    // Pending include materialization is internal-only. If some caller still
    // tries to validate it as a theorem-facing include path, fail closed
    // immediately rather than reporting a live transitional discharge state.
    ProofDischargeAccumulator discharge;
    discharge.Fail(ProofObligationKind::IncludePendingMaterializationClassified,
                   ProofFailureReason::PendingMaterialization);
    return discharge.Finish();
  }

  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory =
      BuildAcceptancePathInventory(currentPath);

  // Common include-preserving baseline: the accepted path must be classified,
  // mapped to a future proof target, and backed by an actual include patch
  // shape before path-specific witness obligations are checked below.
  RequireAcceptedPathBaseline(discharge, inventory);
  discharge.Require(patch != nullptr,
                    ProofObligationKind::IncludePatchShapeTracked,
                    ProofFailureReason::MissingIncludePatchShape);
  if (!patch)
    return discharge.Finish();

  // Every include-preserving path needs an anchor witness. The switch below
  // refines this generic requirement into the exact witness kind and fields
  // required by each accepted include path.
  discharge.Require(witness &&
                        witness->evidence != IncludeAnchorEvidenceKind::Unknown,
                    ProofObligationKind::IncludeAnchorWitnessTracked,
                    ProofFailureReason::MissingIncludeAnchorWitness);

  switch (currentPath) {
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
    // Replacement/deletion over an existing include must be tied to concrete
    // mapped header tokens and the corresponding source byte range.
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
    // Conditional-boundary insertion is zero-width in A and must be anchored to
    // the selected conditional arm boundary byte.
    RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness,
        IncludeAnchorEvidenceKind::SelectedConditionalBoundary,
        ProofObligationKind::IncludeSelectedConditionalBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeSelectedConditionalBoundaryWitness);
    discharge.Require(
        witness && witness->hasCondArmId,
        ProofObligationKind::IncludeSelectedConditionalBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeSelectedConditionalBoundaryWitness);
    break;

  case AcceptedPathKind::IncludeInsertChildBoundary:
    // Child-boundary insertion is also zero-width, but the witness must name
    // the child include whose boundary supplies the insertion anchor.
    RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::ChildBoundary,
        ProofObligationKind::IncludeChildBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeChildBoundaryWitness);
    discharge.Require(witness && witness->hasChildIncludeId,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);
    break;

  case AcceptedPathKind::IncludeInsertRightNeighborPP:
    // Right-neighbor insertion anchors before a known preprocessor token. The
    // anchor byte and neighbor PP token together identify the stable insertion
    // point.
    RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::RightNeighborPP,
        ProofObligationKind::IncludeRightNeighborWitnessTracked,
        ProofFailureReason::MissingIncludeRightNeighborWitness);
    discharge.Require(witness && witness->hasNeighborPP,
                      ProofObligationKind::IncludeRightNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeRightNeighborWitness);
    break;

  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
    // Left-neighbor insertion anchors after a known preprocessor token. As with
    // the right-neighbor case, require both the byte anchor and the neighbor PP
    // identity so the insertion point is theorem-facing.
    RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::LeftNeighborPP,
        ProofObligationKind::IncludeLeftNeighborWitnessTracked,
        ProofFailureReason::MissingIncludeLeftNeighborWitness);
    discharge.Require(witness && witness->hasNeighborPP,
                      ProofObligationKind::IncludeLeftNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeLeftNeighborWitness);
    break;

  case AcceptedPathKind::IncludeInsertDeclBoundary:
    // Declaration-boundary insertion is anchored at the end of the declaration
    // header range. Requiring anchorByte == declHeaderE prevents a witness from
    // naming the right range but anchoring at a different byte.
    RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::DeclBoundary,
        ProofObligationKind::IncludeDeclBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeDeclBoundaryWitness);
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
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    // Non-include-preserving paths have no additional obligations in this
    // validator. The common path-classification checks above still record any
    // mismatch if such a path reaches this function unexpectedly.
    break;
  }

  return discharge.Finish();
}

bool RefoldEngine::IsAcceptedIncludeRealizationEvidenceKind(
    IncludeRealizationEvidenceKind kind) const {
  // Only realized B-cover envelopes are theorem-facing include realization
  // evidence; Unknown is an undischarched realization witness.
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

  // Common include-realization baseline: the path must be classified, mapped to
  // a future proof target, and belong to one of the declared include
  // realization proof classes.
  RequireAcceptedPathBaseline(discharge, inventory);
  discharge.Require(
      currentPath == AcceptedPathKind::IncludeRealizationInlineFromB ||
          currentPath == AcceptedPathKind::IncludeMaterializedExpansion,
      ProofObligationKind::AcceptedPathClassified,
      ProofFailureReason::MissingAcceptedPathClassification);

  if (currentPath == AcceptedPathKind::IncludeRealizationInlineFromB) {
    // Inline include realization is discharged only when the witness records
    // either the canonical B-envelope mapping or the deterministic
    // consensus-rescue envelope. Any other evidence kind is outside the
    // declared domain for this proof class.
    discharge.Require(
        witness && IsAcceptedIncludeRealizationEvidenceKind(witness->evidence),
        ProofObligationKind::IncludeRealizationWitnessTracked,
        ProofFailureReason::MissingIncludeRealizationWitness);

    // The witness must identify the include being realized, the A-side cover
    // being replaced, and the B-side token envelope that supplies the
    // realization text.
    discharge.Require(witness && witness->hasIncludeId &&
                          witness->includeId != 0,
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
    // Materialized include expansion is already represented by the accepted
    // path classification. It has no inline B-envelope witness obligations.
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

  // TU anchor validation applies only to the two accepted TU insertion-anchor
  // classes. Other accepted paths may still reach this function accidentally,
  // but they must fail the classification obligation rather than being treated
  // as anchor proofs.
  const bool classified =
      currentPath == AcceptedPathKind::TUExactSlotBoundary ||
      currentPath == AcceptedPathKind::TUProvableInsertionAnchor;
  discharge.Require(classified, ProofObligationKind::TUAnchorPathClassified,
                    ProofFailureReason::MissingTUAnchorClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);

  // Every accepted TU anchor path must carry an explicit local witness
  // describing which deterministic anchor source succeeded, plus the PP-gap and
  // TU-byte location where the insertion is anchored.
  discharge.Require(witness &&
                        witness->evidence != TUAnchorEvidenceKind::Unknown,
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
    // Exact-slot anchors are the strongest TU anchor class: the witness must
    // name the exact slot, prove the PP gap matched exactly, and preserve the
    // slot kind for auditability.
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
    // Provable insertion anchors are a family of weaker local anchors. Each
    // evidence kind has its own minimal witness fields; the common PP-gap/TU-byte
    // obligations above are not enough by themselves.
    discharge.Require(TUAnchorWitnessHasProvableEvidence(*witness),
                      ProofObligationKind::TUProvableEvidenceTracked,
                      ProofFailureReason::MissingTUProvableAnchorWitness);

    // Ordinary TU anchors must be outside include coverage so they do not
    // silently target materialized include text. Include-directive boundaries
    // are the explicit exception because the directive itself supplies the
    // include-aware anchor.
    discharge.Require(witness->outsideIncludeCoverage ||
                          witness->evidence ==
                              TUAnchorEvidenceKind::IncludeDirectiveBoundary,
                      ProofObligationKind::TUOutsideIncludeCoverageTracked,
                      ProofFailureReason::MissingTUOutsideIncludeCoverageProof);

    // Corroborated neighbor anchors rely on stable ownership across both
    // neighbors, so require the recorded owner-depth stability proof.
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
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
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
    // Non-TU-anchor paths have no path-specific obligations here; the common
    // classification check above records the failure if one reaches this
    // validator.
    break;
  }

  return discharge.Finish();
}

std::string
RefoldEngine::FormatTUAnchorWitness(const TUAnchorWitness &witness) const {
  return formatv(
             "kind={0} ppGap={1}{2} tuByte={3}{4} slotId={5} slotKind={6} "
             "macroId={7} leftPP={8}{9} rightPP={10}{11} outsideInclude={12} "
             "ownerDepthStable={13} exactPP={14}",
             witness.evidence, witness.ppGap,
             witness.hasPPGap ? "" : "(missing)", witness.tuByte,
             witness.hasTUByte ? "" : "(missing)", witness.slotId,
             witness.slotKind.empty() ? StringRef("(none)")
                                      : StringRef(witness.slotKind),
             witness.macroId, witness.leftNeighborPP,
             witness.hasLeftNeighbor ? "" : "(missing)",
             witness.rightNeighborPP,
             witness.hasRightNeighbor ? "" : "(missing)",
             witness.outsideIncludeCoverage ? 1 : 0,
             witness.ownerDepthStable ? 1 : 0, witness.exactPPMatch ? 1 : 0)
      .str();
}

std::string RefoldEngine::FormatIncludeAnchorWitness(
    const IncludeAnchorWitness &witness) const {
  return formatv(
             "kind={0} anchorByte={1}{2} byteRange=[{3},{4}){5} firstPP={6}{7} "
             "lastPP={8}{9} neighborPP={10}{11} condArm={12}{13} "
             "childInc={14}{15} "
             "decl=[{16},{17}){18}",
             witness.evidence, witness.anchorByte,
             witness.hasAnchorByte ? "" : "(missing)", witness.startByte,
             witness.endByte, witness.hasByteRange ? "" : "(missing)",
             witness.firstPP, witness.hasFirstPP ? "" : "(missing)",
             witness.lastPP, witness.hasLastPP ? "" : "(missing)",
             witness.neighborPP, witness.hasNeighborPP ? "" : "(missing)",
             witness.condArmId, witness.hasCondArmId ? "" : "(missing)",
             witness.childIncludeId,
             witness.hasChildIncludeId ? "" : "(missing)", witness.declHeaderB,
             witness.declHeaderE, witness.hasDeclHeaderRange ? "" : "(missing)")
      .str();
}

std::string RefoldEngine::FormatIncludeRealizationWitness(
    const IncludeRealizationWitness &witness) const {
  return formatv("kind={0} includeId={1}{2} aCover=[{3},{4}){5} "
                 "bTokEnvelope=[{6},{7}){8}",
                 witness.evidence, witness.includeId,
                 witness.hasIncludeId ? "" : "(missing)", witness.aCoverBegin,
                 witness.aCoverEnd, witness.hasACover ? "" : "(missing)",
                 witness.bTokBegin, witness.bTokEnd,
                 witness.hasBTokenEnvelope ? "" : "(missing)")
      .str();
}

std::string
RefoldEngine::FormatProofSummaryAuditCore(const ProofSummary &summary) const {
  return formatv("inventory={0} lattice={1} completeness={2} theoremDomain={3} "
                 "discharge={4}",
                 FormatAcceptancePathInventory(summary.inventory),
                 FormatGlobalSelectionLattice(summary.lattice),
                 FormatCompletenessContract(summary.completeness),
                 FormatTheoremDomainContract(summary.theoremDomain),
                 FormatProofDischargeRecord(summary.discharge))
      .str();
}

void RefoldEngine::AppendProofSummaryWitnessAudit(
    std::string &artifact, const ProofSummary &summary, bool firstOnly) const {
  if (summary.hasTUAnchorWitness) {
    artifact += formatv(" tuAnchor={0}",
                        FormatTUAnchorWitness(summary.tuAnchorWitness))
                    .str();
    if (firstOnly)
      return;
  }
  if (summary.hasIncludeAnchorWitness) {
    artifact +=
        formatv(" includeAnchor={0}",
                FormatIncludeAnchorWitness(summary.includeAnchorWitness))
            .str();
    if (firstOnly)
      return;
  }
  if (summary.hasIncludeRealizationWitness) {
    artifact +=
        formatv(" includeRealization={0}",
                FormatIncludeRealizationWitness(summary.includeRealizationWitness))
            .str();
    if (firstOnly)
      return;
  }
  if (summary.hasTerminalFallbackWitness) {
    artifact += formatv(" terminalOutOfDomain={0}",
                        summary.terminalFallbackWitness)
                    .str();
  }
}

RefoldEngine::TerminalFallbackWitness
RefoldEngine::BuildTerminalFallbackWitness() const {
  TerminalFallbackWitness witness;
  witness.kind = terminalFallbackKind_;
  witness.requestCount = terminalFallbackRequestCount_;

  // Preserve the first recorded terminal-fallback reason as the primary audit
  // witness; later reasons remain diagnostic context outside this compact
  // theorem-facing record.
  if (!terminalFallbackReasons_.empty()) {
    witness.hasPrimaryReason = true;
    witness.primaryReason = terminalFallbackReasons_.front();
  }
  return witness;
}

bool RefoldEngine::IsOwnerUnresolvedNoTUAnchorOutOfDomain(
    const diffutils::Hunk &h, StringRef tuPath, const Owner &owner,
    bool mapsToTU) const {
  // This is the explicit out-of-domain predicate for hunks whose owner could
  // not be resolved and whose TU-level fallback witnesses are also unavailable.
  // It must remain derived-only: by the time this helper returns true, all
  // deterministic owner, include-boundary, TU-anchor, and TU-byte-span searches
  // have already failed. Do not infer ownership from proximity here.
  if (owner.kind != OwnerKind::Unknown)
    return false;

  if (h.isInsertOnly()) {
    // Pure insertions have extra deterministic witnesses: they may belong to a
    // parent include boundary, a provable TU insertion anchor, or a zero-width
    // TU byte span. Only if all of those are absent is the insertion outside
    // the declared refolding domain.
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

  // Non-insertion hunks are in-domain if the token map or TU byte-span mapping
  // can still witness the affected range. Without either, there is no declared
  // theorem-facing owner or TU anchor for this hunk.
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

  // This detail string is emitted only after normal owner resolution has failed
  // to produce a macro/include/TU witness. Record those exhausted search spaces
  // explicitly so the terminal fallback explains the domain wall in proof
  // terms, not merely as `OwnerKind::Unknown`.
  const bool macroOwnerExhausted = true;
  const bool includeOwnerExhausted = owner.kind != OwnerKind::Include;

  // For pure insertions, check whether the insertion could still be explained
  // by an include-boundary parent. If present, this is a deterministic include
  // witness, not an out-of-domain owner-unresolved case.
  const RefoldModel::IncludeItem *boundaryInc = nullptr;
  if (isInsertion)
    boundaryInc = BoundaryParentIncludeForPureInsertion(h);
  const bool hasBoundaryInclude = boundaryInc != nullptr;

  // Also test the TU insertion-anchor path. A provable TU anchor keeps an
  // otherwise ownerless insertion inside the declared refolding domain.
  std::optional<uint64_t> provableInsertionAnchor;
  if (isInsertion)
    provableInsertionAnchor = FindProvableTUInsertionAnchor(h.aStart, tuPath);
  const bool hasProvableInsertionAnchor = provableInsertionAnchor.has_value();

  // Finally, check whether the hunk can be represented directly as a TU byte
  // span. This is the last generic TU witness before the edit is declared
  // outside the owner/TU-anchor domain.
  std::optional<std::pair<uint64_t, uint64_t>> tuSpan =
      TUByteSpan(h.aStart, h.aEnd, tuPath);
  const bool hasTUByteSpan = tuSpan.has_value();
  const bool declaredDomainWall =
      IsOwnerUnresolvedNoTUAnchorOutOfDomain(h, tuPath, owner, mapsToTU);

  // These insertion-only fields make the diagnostic precise about which
  // insertion witnesses were attempted and whether each one was absent.
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

  return llvm::formatv("edit #{0} A=[{1},{2}) insert={3} owner={4} "
                       "macroOwnerExhausted={5} "
                       "includeOwnerExhausted={6} mapsToTU={7} TUByteSpan={8} "
                       "hasTUByteSpan={9} "
                       "exactSlot={10} insertionAnchor={11} "
                       "hasProvableInsertionAnchor={12} "
                       "boundaryParentInclude={13} hasBoundaryInclude={14} "
                       "declaredDomainWall={15}",
                       hunkIndex, h.aStart, h.aEnd, isInsertion ? "yes" : "no",
                       toString(owner.kind), macroOwnerExhausted ? "yes" : "no",
                       includeOwnerExhausted ? "yes" : "no",
                       mapsToTU ? "yes" : "no", tuSpanStr,
                       hasTUByteSpan ? "yes" : "no", exactSlot, insertionAnchor,
                       hasProvableInsertionAnchor ? "yes" : "no",
                       boundaryParent, hasBoundaryInclude ? "yes" : "no",
                       declaredDomainWall ? "yes" : "no")
      .str();
}

std::string RefoldEngine::FormatAcceptancePathInventory(
    const AcceptancePathInventory &inventory) const {
  return formatv("currentPath={0} support={1} futureTarget={2}",
                 inventory.currentPath, inventory.support,
                 inventory.futureTarget)
      .str();
}

std::string RefoldEngine::FormatProofDischargeRecord(
    const ProofDischargeRecord &record) const {
  return formatv("status={0} evaluated={1} satisfied={2} failedObligation={3} "
                 "failureReason={4}",
                 record.status, record.obligationsEvaluated,
                 record.obligationsSatisfied, record.failedObligation,
                 record.failureReason)
      .str();
}

std::string RefoldEngine::FormatGlobalSelectionLattice(
    const GlobalSelectionLattice &lattice) const {
  return formatv("domain={0} mergeLaw={1} conflictLaw={2}", lattice.domain,
                 lattice.mergeLaw, lattice.conflictLaw)
      .str();
}

std::string RefoldEngine::FormatCompletenessContract(
    const CompletenessContract &contract) const {
  if (contract.hasExplicitExclusion) {
    const TerminalFallbackWitness witness{contract.explicitExclusion, 0, false,
                                          std::string()};
    return formatv("coverage={0} expectation={1} declaredTarget={2} counts={3} "
                   "exclusion={4}",
                   contract.coverage, contract.expectation,
                   contract.declaredTarget,
                   contract.countsTowardDeclaredCoverage ? 1 : 0, witness)
        .str();
  }
  return formatv("coverage={0} expectation={1} declaredTarget={2} counts={3}",
                 contract.coverage, contract.expectation,
                 contract.declaredTarget,
                 contract.countsTowardDeclaredCoverage ? 1 : 0)
      .str();
}

std::string RefoldEngine::FormatTheoremDomainContract(
    const TheoremDomainContract &contract) const {
  if (contract.hasExplicitExclusion) {
    const TerminalFallbackWitness witness{contract.explicitExclusion, 0, false,
                                          std::string()};
    return formatv("kind={0} inDeclaredDomain={1} counts={2} declaredTarget={3}"
                   " exclusion={4}",
                   contract.kind, contract.inDeclaredDomain ? 1 : 0,
                   contract.countsTowardCompleteness ? 1 : 0,
                   contract.declaredTarget, witness)
        .str();
  }
  return formatv("kind={0} inDeclaredDomain={1} counts={2} declaredTarget={3}",
                 contract.kind, contract.inDeclaredDomain ? 1 : 0,
                 contract.countsTowardCompleteness ? 1 : 0,
                 contract.declaredTarget)
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
  std::string audit = FormatProofSummaryAuditCore(summary);
  AppendProofSummaryWitnessAudit(audit, summary, /*firstOnly=*/true);
  return audit;
}


std::string RefoldEngine::FormatAcceptedResultCandidate(
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

  AppendProofSummaryWitnessAudit(artifact, candidate.proofSummary,
                                 /*firstOnly=*/false);
  if (candidate.hasPayloadPreview) {
    artifact += formatv(" payload='{0}'", candidate.payloadPreview).str();
  }

  const std::string artifactSuffix =
      artifact.empty() ? std::string()
                       : formatv(" artifact={0}", artifact).str();
  return formatv("kind={0} class={1} realization={2} preference={3} "
                 "surfaceDisposition={4} {5}{6}",
                 candidate.kind, candidate.proofSummary.acceptedClass,
                 candidate.proofSummary.realizationMode,
                 candidate.proofSummary.preference,
                 candidate.proofSummary.surfaceDisposition,
                 FormatProofSummaryAuditCore(candidate.proofSummary),
                 artifactSuffix)
      .str();
}

std::string RefoldEngine::FormatMacroPatchAudit(const MacroPatch &patch) const {
  const ProofSummary summary = ClassifyMacroPatchProof(patch);
  return formatv(
             "proofKind={0} topClass={1} realization={2} preference={3} "
             "surfaceDisposition={4} {5} validated={6} struct={7} "
             "proofRoot={8} subtreeCert={9} "
             "leaf={10} witnesses={11} invCerts={12} formalCerts={13} "
             "argCerts={14} liftChains={15} liftSteps={16} rootMerges={17} "
             "lexicalBridge={18} paste={19} wrappers={20} stringify={21} "
             "wideStringify={22} childSyntax={23} rawInvocation={24} "
             "passthrough={25} bridgeSensitive={26} "
             "deferredPasteDischarged={27} admissible={28} "
             "pasteReplayValidated={29} expRootN={30} "
             "deferredRootN={31} bridgeFormalN={32} expRoot={33} "
             "deferredRootArgs={34} bridgeFormals={35} wholeCoverA=[{36},{37}) "
             "wholeCoverBraw=[{38},{39}) wholeCoverBadj=[{40},{41})",
             patch.proofKind, summary.acceptedClass, summary.realizationMode,
             summary.preference, summary.surfaceDisposition,
             FormatProofSummaryAuditCore(summary), patch.proofValidated ? 1 : 0,
             patch.structurePreserving ? 1 : 0, patch.proofRootMacroId,
             patch.subtreeCertBacked ? 1 : 0, patch.subtreeLeafMacroId,
             patch.subtreeWitnessCount, patch.subtreeInvocationCertCount,
             patch.subtreeFormalCertCount, patch.subtreeArgCertCount,
             patch.subtreeLiftChainCount, patch.subtreeLiftStepCount,
             patch.subtreeRootMergeCount, patch.subtreeUsesLexicalBridge ? 1 : 0,
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
             stringutils::showWsWithClip(patch.subtreeExpectedRootFormalSummary,
                                         160),
             stringutils::showWsWithClip(patch.subtreeDeferredRootArgSummary,
                                         160),
             stringutils::showWsWithClip(
                 patch.subtreeBridgeSensitiveFormalSummary, 160),
             patch.wholeCoverALo, patch.wholeCoverAHi, patch.wholeCoverBRawLo,
             patch.wholeCoverBRawHi, patch.wholeCoverBAdjLo,
             patch.wholeCoverBAdjHi)
      .str();
}

std::optional<std::string> RefoldEngine::BuildWholeCoverReplacementText(
    const RefoldModel::MacroInvocation &m) const {
  // Reuse the same whole-cover planning path used by patch construction so the
  // returned replacement text obeys the same clipping/envelope policy.
  auto plan = ComputeWholeCoverPlan(m);
  if (!plan)
    return std::nullopt;
  return plan->clippedText;
}

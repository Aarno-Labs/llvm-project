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
  const MacroPatchProof &proof = patch.proof;
  const AcceptancePathInventory inventory =
      InventoryMacroPatchProofAcceptancePath(proof);

  RequireAcceptedPathBaseline(discharge, inventory);
  discharge.Require(proof.proofRootMacroId != 0,
                    ProofObligationKind::ProofRootTracked,
                    ProofFailureReason::MissingProofRoot);

  const RefoldModel::MacroInvocation *root =
      proof.proofRootMacroId ? FindMacroInvocationById(proof.proofRootMacroId)
                             : nullptr;
  discharge.Require(root != nullptr,
                    ProofObligationKind::MacroProofRootResolved,
                    ProofFailureReason::MissingMacroProofRootResolution);
  if (requireTopLevelRoot && root) {
    discharge.Require(GetRootMacroId(root->id) == root->id,
                      ProofObligationKind::MacroProofRootIsTopLevel,
                      ProofFailureReason::NonTopLevelMacroProofRoot);
  }

  switch (proof.kind) {
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
      const bool replayValidated =
          proof.paste && proof.paste->replayValidated;
      discharge.Require(hasProducerWitness || replayValidated,
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
    discharge.Require(proof.subtree && proof.subtree->backed,
                      ProofObligationKind::MacroSubtreeCertificateTracked,
                      ProofFailureReason::MissingSubtreeCertificate);
    if (proof.subtree && proof.subtree->backed) {
      discharge.Require(proof.subtree->admissible,
                        ProofObligationKind::SubtreeAdmissibilityTracked,
                        ProofFailureReason::MissingSubtreeAdmissibility);
    }
    break;

  case MacroPatchProofKind::CallChainSuffix:
    discharge.Require(
        proof.callChain &&
            proof.callChain->rootMacroId == proof.proofRootMacroId &&
            proof.callChain->callsiteMacroId == patch.macroId &&
            patch.macroId == proof.proofRootMacroId,
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
  const MacroPatchProof &proof = patch.proof;
  const AcceptancePathInventory inventory =
      InventoryMacroPatchProofAcceptancePath(proof);

  // Invocation-realization patches still need the common accepted-path
  // metadata, but unlike invocation-preserving proofs they are expected to
  // realize expansion text rather than preserve source invocation structure.
  RequireAcceptedPathBaseline(discharge, inventory);
  discharge.Require(proof.proofRootMacroId != 0,
                    ProofObligationKind::ProofRootTracked,
                    ProofFailureReason::MissingProofRoot);

  if (proof.kind == MacroPatchProofKind::WholeCoverRealization) {
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
    discharge.Require(boundsTracked,
                      ProofObligationKind::WholeCoverBoundsTracked,
                      ProofFailureReason::MissingWholeCoverBounds);
    // Phase 8e deletes the nested/coarse-span whole-cover fallback.  A macro
    // realization must now carry the direct self-contained owner-cover proof
    // that was checked before BuildMacroWholeCoverOwnerRealization() stamped the
    // shared OwnerRealizationProof.
    discharge.Require(patch.wholeCoverSelfContained,
                      ProofObligationKind::WholeCoverContainmentTracked,
                      ProofFailureReason::MissingWholeCoverContainment);
    discharge.Require(boundaryAccountingTracked,
                      ProofObligationKind::WholeCoverBoundaryAccountingTracked,
                      ProofFailureReason::MissingWholeCoverBoundaryAccounting);
    discharge.Require(
        proof.ownerRealization &&
            proof.ownerRealization->evidence ==
                OwnerRealizationEvidenceKind::MacroWholeCover,
        ProofObligationKind::OwnerRealizationWitnessTracked,
        ProofFailureReason::MissingOwnerRealizationWitness);
  } else {
    // Non-whole-cover realization paths are admitted by their explicit macro
    // proof kind plus a typed state-stability witness when the proof depends on
    // stabilizing preprocessor state. There is no separate legacy validation
    // bit in the theorem vocabulary.
    if (proof.kind == MacroPatchProofKind::CounterLiteral) {
      const SuffixStabilityWitnessKind kind =
          proof.suffixStability ? proof.suffixStability->kind
                                : SuffixStabilityWitnessKind::None;
      const bool counterWitnessTracked =
          proof.suffixStability &&
          SuffixStabilityWitnessNamesComponent(*proof.suffixStability,
                                               OwnerStateComponent::Counter) &&
          (kind == SuffixStabilityWitnessKind::Literalization ||
           kind == SuffixStabilityWitnessKind::OwnerMaterialization ||
           kind == SuffixStabilityWitnessKind::ClosureWidening);
      discharge.Require(counterWitnessTracked,
                        ProofObligationKind::CounterStateWitnessTracked,
                        ProofFailureReason::MissingCounterStateWitness);
    }
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

  case AcceptedPathKind::IncludeInsertChildBoundary: {
    // Phase 8d classification: IncludeInsertionByChildBoundary is a declared
    // include-preserving proof, not a legacy fallback.  The accepted patch must
    // be a zero-width insertion owned by a concrete parent include, and the
    // witness must name a direct child include whose spelled directive boundary
    // is exactly the recorded anchor byte.  This keeps the theorem obligation
    // owner-local and prevents an arbitrary child id from masquerading as a
    // stable parent-header insertion point.
    RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::ChildBoundary,
        ProofObligationKind::IncludeChildBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeChildBoundaryWitness);
    discharge.Require(patch->include != nullptr,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);
    discharge.Require(witness && witness->hasChildIncludeId,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);

    bool childBoundaryMatchesParent = false;
    if (patch->include && witness && witness->hasChildIncludeId &&
        witness->hasAnchorByte) {
      for (const auto &child : model_.GetIncludes()) {
        if (child.id != witness->childIncludeId)
          continue;
        const bool directChildOfPatchOwner =
            child.parent && *child.parent == patch->include->id;
        const bool anchorIsSpelledChildBoundary =
            witness->anchorByte == child.siteB ||
            witness->anchorByte == child.siteE;
        childBoundaryMatchesParent =
            directChildOfPatchOwner && anchorIsSpelledChildBoundary;
        break;
      }
    }
    discharge.Require(childBoundaryMatchesParent,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);
    break;
  }

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
  case AcceptedPathKind::TUIncludeClosureEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    // Non-include-preserving paths have no additional obligations in this
    // validator. The common path-classification checks above still record any
    // mismatch if such a path reaches this function unexpectedly.
    break;
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
  case AcceptedPathKind::TUIncludeClosureEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    // Non-TU-anchor paths have no path-specific obligations here; the common
    // classification check above records the failure if one reaches this
    // validator.
    break;
  }

  return discharge.Finish();
}

RefoldEngine::TerminalFallbackWitness
RefoldEngine::BuildTerminalFallbackWitness() const {
  TerminalFallbackWitness witness;

  // Phase 4D makes the ordered typed requests the single source of truth.
  // There is no branch-local fallback boolean, no primary failure scalar, and
  // no separate proof-failure vector mirror.  The primary failed obligation is
  // simply the first classified request failure copied into this witness.
  witness.proofFailures.reserve(terminalFallbackRequests_.size());
  for (const TerminalFallbackRequest &request : terminalFallbackRequests_) {
    if (IsClassifiedTerminalFallbackProofFailure(request.failure))
      witness.proofFailures.push_back(request.failure);
  }

  if (witness.proofFailures.empty()) {
    // R4 forbids deriving the failed obligation from an aggregate terminal
    // kind.  If this state is ever reached, the bug is the missing
    // caller-supplied proof failure itself, so report a theorem-audit
    // invariant violation rather than inventing an owner/state reason here.
    witness.proofFailures.push_back(MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied,
        TerminalFallbackFailureReason::TheoremAuditInvariantViolation,
        TerminalFallbackFailureContext::ForStateComponent(
            "terminalFallbackWitness")));
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
                       owner.kind, macroOwnerExhausted ? "yes" : "no",
                       includeOwnerExhausted ? "yes" : "no",
                       mapsToTU ? "yes" : "no", tuSpanStr,
                       hasTUByteSpan ? "yes" : "no", exactSlot, insertionAnchor,
                       hasProvableInsertionAnchor ? "yes" : "no",
                       boundaryParent, hasBoundaryInclude ? "yes" : "no",
                       declaredDomainWall ? "yes" : "no")
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

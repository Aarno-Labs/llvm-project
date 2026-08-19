set(LLVM_LINK_COMPONENTS Support)

# The refold engine is built as a library so its proof vocabulary and pure
# coordinate theorems can be exercised by unit tests.  Several terminal-
# fallback obligations are internal-invariant assertions that no input can
# reach, so a lit test cannot pin them; the library boundary is what makes
# them testable at all.  The tool itself is a thin main over this library.
add_clang_library(clangRefold STATIC
  sideband/RefoldSidebandPragmaEdits.cpp
  source/DiffAlgorithms.cpp
  source/RefoldAlignmentDiagnostic.cpp
  source/RefoldAlignmentSemanticResolver.cpp
  source/RefoldLegacyAlignmentDiagnostic.cpp
  line-control/FinalLineControlModel.cpp
  line-control/LineDirectiveInserter.cpp
  line-control/RefoldLineControlFilename.cpp
  line-control/RefoldLineControlRewrite.cpp
  line-control/RefoldNoLinesPruning.cpp
  edit/RefoldExpansionFallbackPlanner.cpp
  edit/RefoldBInsertionLedger.cpp
  core/RefoldFinalAssemblyVerifier.cpp
  core/RefoldEngine.cpp
  core/RefoldAlignmentSemanticIntegration.cpp
  core/RefoldLangOptions.cpp
  core/RefoldPreprocessRecheck.cpp
  core/RefoldServiceGraphBuilder.cpp
  line-control/RefoldLineObserverLayout.cpp
  include/RefoldHeaderIncludeEditPlanner.cpp
  include/RefoldIncludeInsertionPlanner.cpp
  include/RefoldIncludeSubtreeWorkClassifier.cpp
  include/RefoldIncludeMaterializer.cpp
  include/RefoldIncludeMaterializationScheduler.cpp
  include/RefoldPragmaOnceGuardRewriter.cpp
  edit/RefoldTextEditAssembler.cpp
  util/RefoldPathCanonicalization.cpp
  util/RefoldPathIdentity.cpp
  source/RefoldTokenTextAnalysis.cpp
  source/RefoldSourceMapper.cpp
  source/RefoldToken.cpp
  source/RefoldTokenDiffPlanner.cpp
  source/RefoldTokenlessSourceProjection.cpp
  source/RefoldMixedOwnerTilingPlanner.cpp
  source/RefoldPreprocessingDirectiveScanner.cpp
  source/RefoldPreprocessingStructureIndex.cpp
  source/RefoldPreprocessingStructureIndexProvider.cpp
  source/RefoldSourceGapProof.cpp
  source/RefoldStructuralHunkDispatcher.cpp
  macro/RefoldMacroTopology.cpp
  macro/RefoldMacroReplay.cpp
  macro/RefoldMacroArgsOnlyTemplateSolver.cpp
  macro/RefoldMacroDefinitionTapeSolver.cpp
  macro/RefoldMacroGeneratedLeafReplayEngine.cpp
  macro/RefoldMacroOccurrenceProofValidator.cpp
  macro/RefoldMacroPasteArgumentBuilder.cpp
  macro/RefoldMacroPatchPlanner.cpp
  macro/RefoldMacroArgsOnlyWholeCoverPhase.cpp
  macro/RefoldMacroDAGLeafDiscoveryPhase.cpp
  macro/RefoldMacroDAGLiftingPhase.cpp
  macro/RefoldMacroDAGCandidateValidator.cpp
  macro/RefoldMacroDAGInvertibilitySolver.cpp
  macro/RefoldMacroDAGStructuredLifter.cpp
  macro/RefoldMacroDAGSubtreeCertifier.cpp
  macro/RefoldMacroDAGTextPrimitives.cpp
  macro/RefoldMacroFinalCandidateSelector.cpp
  macro/RefoldMacroPatchProofCertifier.cpp
  macro/RefoldMacroPatchReusePhase.cpp
  macro/RefoldMacroSelectorSubstitutionPhase.cpp
  macro/RefoldMacroStandardArgsOnlyPatchBuilder.cpp
  macro/RefoldMacroStandardArgsOnlyOccurrence.cpp
  macro/RefoldMacroStandardArgsOnlyProof.cpp
  macro/RefoldMacroStandardArgsOnlyGeneratedReplay.cpp
  macro/RefoldMacroRecursiveTupleGeneratedReplay.cpp
  macro/RefoldMacroSubtreeReplayValidator.cpp
  macro/RefoldMacroReplayStabilityValidator.cpp
  macro/RefoldMacroGeneratedCalleeReplayEngine.cpp
  macro/RefoldMacroTupleHelpers.cpp
  macro/RefoldMacroWholeCoverOrchestrator.cpp
  macro/RefoldMacroStateRepairPlanner.cpp
  core/RefoldOwnerClassifier.cpp
  edit/RefoldTUEditPlanner.cpp
  edit/RefoldFinalTUEmissionPlanner.cpp
  edit/RefoldTUAnchorProof.cpp
  proof/RefoldTerminalProofSink.cpp
  proof/RefoldTheoremAudit.cpp
  macro/RefoldArgTextRecovery.cpp
  macro/RefoldCounterStabilization.cpp
  include/RefoldIncludeReplayProof.cpp
  proof/RefoldAcceptancePathClassifier.cpp
  proof/RefoldAcceptedCandidateBuilder.cpp
  proof/RefoldAcceptedResultRanker.cpp
  proof/RefoldMacroPatchProofClassifier.cpp
  proof/RefoldOwnerRealizationProofBuilder.cpp
  proof/RefoldProofLattice.cpp
  proof/RefoldProofSummaryBuilder.cpp
  proof/RefoldStructuralGapCrossingProof.cpp
  proof/RefoldStructuralHunkTilingProof.cpp
  proof/RefoldWitnessClassifier.cpp
  proof/RefoldWitnessEquivalenceKeyBuilder.cpp
  proof/RefoldWitnessResolver.cpp
  proof/RefoldWitnessTrace.cpp
  proof/RefoldOwnerStateProof.cpp
  line-control/RefoldLineControlProof.cpp
  macro/RefoldMacroStateProof.cpp
  proof/RefoldSidebandReplayProof.cpp
  proof/RefoldNeutralityProof.cpp
  proof/RefoldPragmaTaxonomy.cpp
  core/RefoldLog.cpp
  core/RefoldModel.cpp
  util/StringUtils.cpp

  # clang-refold.cpp is the tool's main and is deliberately not part of the
  # library, so this source list is intentionally partial.
  PARTIAL_SOURCES_INTENDED
)

# Link the clang component libraries directly rather than through
# clang_target_link_libraries(), which would resolve to libclang-cpp.so when the
# dylib is enabled.  The tool is deployed by copying the binary into a separate
# tree, so it keeps the static linkage it had before the library split and gains
# no runtime dependency from it.
target_link_libraries(clangRefold PRIVATE
  clangBasic
  clangLex
  clangFrontend
  clangSerialization
)

add_clang_tool(clang-refold
  clang-refold.cpp

  PARTIAL_SOURCES_INTENDED
)

target_link_libraries(clang-refold PRIVATE
  clangRefold
)

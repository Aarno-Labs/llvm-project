# clang-refold callback inventory

Every `std::function` in a clang-refold header, classified by what it actually
does rather than by its comment. Where a comment says "cycle-breaking", the
claim was checked against the construction order in
`RefoldEngine::BuildServiceGraph()` (`core/RefoldServiceGraphBuilder.cpp`) and
against what the target operation reads.

Taken at `38f6ee9a16a4` (49 occurrences in 23 headers) and updated after
roadmap Phase 1 (45 occurrences), Phase 3 (43) and Phase 4 (38). Phase 7
added and removed none; it rechecked every site's line number.
`check_refold_layers.py --metrics` reports the live count. Update this file in
any commit that adds or removes one.

| Class | Count | Meaning | Disposition |
| --- | ---: | --- | --- |
| S — strategy | 9 | A caller-supplied oracle, visitor or policy; the parameter *is* the interface. | Keep. |
| I — indirection | 25 | Forwards to a service or state the consumer could hold directly: the target either exists before the consumer or needs nothing from the owner. | Replace with a reference or a free function. |
| G — genuine cycle | 4 | The target service is constructed after, or owns, the consumer and needs it. | Remove by the named roadmap item, or keep documented here. |

Removed in Phase 1 (four), Phase 3 (two) and Phase 4 (five); all are listed at
the end.

## S — strategy (keep)

| Site | Member | Why it is a real parameter |
| --- | --- | --- |
| `line-control/FinalLineControlModel.h:264` | `FinalSourcePreprocessCallback` | The driver supplies how to re-preprocess a final source. |
| `line-control/FinalLineControlModel.h:285` | `FinalLineControlValidationCallback` | The driver supplies the validation oracle for directive pruning. |
| `line-control/SourceLineDirectiveHelpers.h:141` | `SourceLineDirectiveBuiltinMacroResolver` | Per-caller builtin-macro resolution policy. |
| `line-control/SourceLineDirectiveHelpers.h:141` | `SourceLineDirectiveLogicalLineRewriter` | Per-caller rewrite policy. |
| `macro/RefoldMacroDAGTextPrimitives.h:199` | `emitMatch` | Visitor parameter of `EnumerateTopLevelLiteralMatchesInRefoldText`. |
| `source/RefoldAlignmentSemanticResolver.h:137` | `SimulationCallback` | Dependency inversion: the resolver asks core to realize one candidate map as a nested pass. |
| `source/RefoldAlignmentSemanticResolver.h:157` | `retainWindowOracle` | Same inversion; bounds retained quadratic state per window. |
| `source/RefoldAlignmentSemanticResolver.h:160` | `releaseWindowOracle` | Same inversion. |
| `source/RefoldTokenDiffPlanner.h:168` | `semanticAlignmentResolver` | Optional production resolver; empty for isolated simulations. |

## I — indirection (replace)

**Macro phases → `RefoldMacroPatchPlanner`.** Each forwards to a planner
method that reads no planner-owned state. It either forwards again to an
analysis-layer service or reads only `model`, `lexLang`, `aToks`/`bToks` or
the source mapper. The phases can hold those directly.

| Member | Sites | Planner method reads |
| --- | --- | --- |
| `getMacroInvocationFormalArgContentRanges` | `DAGCandidateValidator.h:210`, `DAGInvertibilitySolver.h:372`, `DAGLeafDiscoveryPhase.h:153`, `DAGLiftingPhase.h:94`, `DAGStructuredLifter.h:255`, `DAGTextPrimitives.h:145`, `FinalCandidateSelector.h:86`, `PatchReusePhase.h:81`, `SelectorSubstitutionPhase.h:77` | forwards to `RefoldMacroActualLayout` (analysis) |
| `macroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof` | `DAGInvertibilitySolver.h:378`, `DAGLiftingPhase.h:103` | forwards to `macro/RefoldMacroReplay.h` (analysis) |
| `resolveFunctionLikeMacroForReplay` | `GeneratedCalleeReplayEngine.h:340`, `GeneratedLeafReplayEngine.h:90` | `model` |
| `resolveFunctionLikeMacroThroughAliasesWithHops` | `GeneratedCalleeReplayEngine.h:347`, `StandardArgsOnlyPatchBuilder.h:101` | `model` |
| `buildInvocationRewriteWithRange` | `GeneratedCalleeReplayEngine.h:354`, `GeneratedLeafReplayEngine.h:101`, `StandardArgsOnlyPatchBuilder.h:108` | `lexLang` |
| `isObjectLikeSingleTokenAlias` | `GeneratedLeafReplayEngine.h:96` | `model` |
| `certifyMacroPatchWholeExpansionBRange` | `StandardArgsOnlyPatchBuilder.h:117` | source mapper |
| `macroPatchOwnerMatches` | `FinalCandidateSelector.h:78` | nothing (pure) |
| `isParenthesizedTuple` | `FinalCandidateSelector.h:82` | `lexLang` |
| `tokenSpellingsEqualToA` / `tokenSpellingsEqualToB` | `SelectorSubstitutionPhase.h:82`, `:86` | `aToks` / `bToks` |
| `validateMergedDirectAndDagRootReplacement` | `SelectorSubstitutionPhase.h:96` | sibling phase `RefoldMacroPatchReusePhase` |

All 25 class-I entries are these macro sites, all under `macro/`.

## G — genuine cycles

| Site | Member | Cycle | Removed by |
| --- | --- | --- | --- |
| `proof/RefoldTerminalProofSink.h:37` | `AuditLegacyAuthorityFn` | the audit files terminal requests into the sink; the sink reports each request back into the audit | Kept (see below) |
| `proof/RefoldTerminalProofSink.h:39` | `NoteTheoremAuditViolationFn` | same | Kept |
| `macro/RefoldMacroArgsOnlyWholeCoverPhase.h:77` | `buildMacroInvocationPatchArgsOnly` | the orchestrator's phases call the planner's args-only entry point, which uses planner-owned builders | Extract the args-only entry point into a service built before the orchestrator |
| `macro/RefoldMacroDAGLeafDiscoveryPhase.h:161` | `buildMacroInvocationPatchArgsOnly` | same | same |

The two terminal-sink hooks stay. The sink audits a request *as it arrives*:
it passes the audit the request's original failure, then normalizes it,
reporting each normalization as it happens. The first hook can write a
`no-legacy-audit` warning at that moment, and the second decides which
violation the audit records as its first. A ledger below both services, read
by the audit afterwards, would move those warnings in the log and could change
which violation counts as first. An organization commit may not change either.

## Removed in Phases 1, 3 and 4

| Former site | Member | How it went |
| --- | --- | --- |
| `proof/RefoldAcceptancePathClassifier.h` | `validateIncludePreservingProof` | The lattice implementation used only the classifier's methods and the model; it moved into the classifier. |
| `proof/RefoldAcceptancePathClassifier.h` | `validateTUAnchorProof` | The classifier calls the shared `validateTUAnchorProof` theorem directly. |
| `proof/RefoldAcceptedResultRanker.h` | `normalizeAcceptedProof` | Indirection: the ranker now borrows `RefoldProofSummaryBuilder`, which depends on `bToks` alone. |
| `proof/RefoldOwnerRealizationProofBuilder.h` | `buildAcceptedPathProofSummary` | Listed here as a genuine cycle, but it was not one. The classifier's dependency on the owner-realization builder was named only in a comment. With it dropped, the classifier is built first and borrowed directly. |
| `proof/RefoldTerminalProofSink.h` | `traceTerminalRequest` | Not a cycle once the witness trace is built first: it reads only the pass flags. The engine now owns it ahead of the sink; the sink and `RefoldProofServices` borrow it. |
| `edit/RefoldExpansionFallbackPlanner.h` | `resetAttemptStats` | Indirection: the planner already held both arguments of `resetRefoldAttemptStats` and calls it directly. |
| `edit/RefoldTextEditAssembler.h` | `lineResyncShouldDeferToConditionalJoin` | The cycle went with its cause (L1). `ApplyResyncOrPend` moved to `RefoldLineObserverLayout`, which answers the deferral itself; the layout depends on the new `RefoldTextEditCertifier`, not the assembler, so it is built first and the assembler borrows it. |
| `edit/RefoldExpansionFallbackPlanner.h` | `applyResyncOrPend` | Indirection (L1): the planner borrows `RefoldLineObserverLayout`. |
| `edit/RefoldExpansionFallbackPlanner.h` | `certifyTextEditMaterializedBTokenRange` | Indirection (L1): the planner borrows `RefoldTextEditCertifier`. |
| `edit/RefoldExpansionFallbackPlanner.h` | `attachAcceptedResultCarrier` | Same. |
| `edit/RefoldExpansionFallbackPlanner.h` | `authorizeTUIncludeClosure` | Same; the planner passes the hook's fixed arguments itself. |

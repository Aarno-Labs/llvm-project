# clang-refold proof architecture

`Architecture.md` says which file may depend on which. This document says how
a refold decision is made: how producer evidence becomes a witness, how a
witness becomes a selectable candidate, and how selected candidates become
emitted source. For each transition it names the subsystem that owns it.

The contract the pipeline enforces is stated once, in the file comment of
`core/RefoldEngine.h` ("Strict-Domain Theorem Contract"). In short: inside the
declared domain, the emitted source must preprocess to exactly the edited
stream B, every emitted edit must normalize to one `TheoremProofClass`, and
everything outside the domain must carry a declared terminal obligation rather
than a guess.

## The seven transitions

| # | Transition | Question it answers | What it produces | Owner |
| ---: | --- | --- | --- | --- |
| 1 | Evidence | What did the producer record, and how do A and B align? | Model records, token maps, hunks, structure census | `model/`, `source/` alignment and census services |
| 2 | Applicability | Which owner may realize this hunk, and must it be split? | Normalized hunks, tiling witnesses, owner, insertion claims | `RefoldEngine::PlanTokenDiff`, tiling planner, owner classifier, B-insertion ledger |
| 3 | Witness | Which theorem family can realize it, and on what evidence? | Patches and edits carrying typed witnesses | Family planners and their certifiers |
| 4 | Normalization | Which single theorem class discharges it? | `AcceptedResultCandidate` with a finalized `ProofSummary` | Acceptance-path classifier, proof-summary builder, accepted-candidate builder |
| 5 | Competition | Which of several valid candidates is chosen? | One selected candidate, or a refusal | Accepted-result ranker, witness resolver, theorem audit |
| 6 | Composition | Do the chosen artifacts combine into one consistent edit set? | Staged, authorized, audited `TextEdit`s | Hunk dispatcher, state-repair planners, text-edit certifier, assembler audits |
| 7 | Emission | What bytes are written, and do they replay B? | Final source, or a failure | Final TU emission planner, assembler, materializer, line-control pruner, verifier, run controller |

The rest of this document takes them in order. Two rules hold at every step:

- **Construction provenance is never proof authority.** `AcceptedPathKind`,
  `AcceptedProofClass` and the `EmissionPathInventory` record how a candidate
  was built. Only `ProofSummary::theoremClass`, finalized by the proof-summary
  builder, says why it is valid, and only that is ranked or emitted.
- **Every stage fails closed.** A stage that cannot discharge its obligation
  rejects the candidate. When nothing else can realize the hunk, it files a
  typed request with the terminal sink. It never widens a guess.

## Where the transitions run

One refold is a sequence of complete passes driven by the run controller.

```text
refoldTranslationUnit()                       core/RefoldRunController.cpp
  loop: build a fresh RefoldEngine from a RefoldPassConfig
    Refold()                                  core/RefoldEngine.cpp
      RunRefoldPass()
        PlanTokenDiff()             1 evidence, 2 applicability
        StageSidebandEdits()        3 witness (sideband pragmas)
        DispatchStructuralHunks()   2 owner, 3 witness, 4 normalize, 5 compete
        FinalizeStructuralResult()  6 composition, 7 assembly
      EnforceTheoremAuditInvariants()         5 audit -> terminal request
      ResolvePostStructuralFallback()         7 terminal carrier, if requested
      PruneFinalLineControlDirectives()       7 final line control
      FinalAssemblyVerifier::Verify()         7 closing check, after a prune
      AuditPreservedLineObserversInFinalOutput()
    controller: verify the assembly; on a proven divergence or a terminal
    request, add owners to ownersMustExpand and run another pass
```

Two kinds of nested pass exist besides the production attempt. An alignment
probe stops after token-diff planning, once alignment resolution has published
its answer. A candidate-map simulation realizes one alignment map completely,
so the semantic resolver can compare real outcomes. Both are configured by
`RefoldPassConfig`; neither emits.

## 1. Evidence

Evidence is what the producer recorded, plus facts derived from it by a
deterministic rule. Nothing is inferred from spelling similarity, token
proximity or candidate order.

| Evidence | Owner |
| --- | --- |
| Producer refold map: tokens, owners, includes, invocations, directives, conditional arms | `model/RefoldModel` (parse, validate, query), `model/RefoldSchema` |
| A and B token streams | `model/RefoldToken` (`lexPPTokens`) |
| A↔B alignment. The certified LCS keeps only anchors that every optimal path uses (forced anchors). | `source/DiffAlgorithms`, driven by `source/RefoldTokenDiffPlanner` |
| Non-forced anchors, restored only after every optimal map is enumerated and realized in isolation, and either all realize one output or one exact least source-transformation class exists | `source/RefoldAlignmentSemanticResolver`, `core/RefoldAlignmentSemanticIntegration.cpp` |
| A/B byte and token coordinate projection | `source/RefoldSourceMapper`, `source/RefoldTokenlessSourceProjection` |
| Census of protected preprocessing structure: directives, pragma operators, conditional arms, bound to producer records | `source/RefoldPreprocessingStructureIndex` (+ `Provider`, `DirectiveScanner`) |
| Macro invocation graph, replay primitives, argument layout | `macro/RefoldMacroTopology`, `macro/RefoldMacroReplay`, `macro/RefoldArgTextRecovery` |
| Owner-state facts: entry, observed, mutated and exit state per owner, and the transition graph | `proof/RefoldOwnerStateProof` |
| Line-control and builtin-observer facts | `line-control/RefoldLineControlProof`, `line-control/LineDirectiveInserter` |
| Pragma classification: what state a pragma changes, and whether it binds to what follows | `proof/RefoldPragmaTaxonomy` |
| Sideband pragma lines in the raw replay streams, removed from the ordinary token diff | `sideband/RefoldSidebandPragmaEdits` |

Evidence services answer questions; they do not edit, order or select. The
structure index says this directly: a producer binding identifies a record, but
it "does not certify that consuming, moving, or reconstructing that interval is
safe". That decision belongs to the proof services below.

Alignment is resolved on demand. The first attempt plans on forced anchors
only. The controller turns resolution on for a later attempt only when a pass
produced evidence that ambiguity limited it, and the answer is memoized for the
run.

## 2. Applicability

Applicability decides which owner may realize a hunk and whether the hunk must
first be split. It happens in `RefoldEngine::PlanTokenDiff()` and at the top of
`DispatchStructuralHunks()`, in this order:

1. **Hunk edge repair.** A replacement hunk whose edge falls inside a macro
   expansion would own only a fragment of it.
   `RepairHunkEdgesOutOfPartiallyOwnedMacroExpansions` either retracts the edge
   across tokens identical in A and B, or widens it to the expansion's own
   boundary.
2. **Structural tiling.** `source/RefoldStructuralHunkTilingPlanner` may split
   one replacement or deletion hunk into a unique ordered partition of owner
   segments and zero-token state-gap edges. It splits only for a proven
   reason (`StructuralTilingReason`): the segments have different realizers, or
   a protected preprocessing interval lies between them, or both. It records a
   `StructuralHunkTilingWitness` and one segment binding per emitted segment in
   engine-owned ledgers. The engine validates that publication before anything
   records a hunk index.
3. **Insertion provenance.** `edit/RefoldBInsertionLedger` records every pure
   B-only insertion and pre-claims the ones emitted standalone, so each B
   insertion is emitted at most once.
4. **Owner classification.** `source/RefoldOwnerClassifier` answers "which
   source owner does this hunk belong to?" (the TU, an include occurrence or a
   macro invocation, among others). `RefoldMacroBoundarySelector`
   (`macro/RefoldMacroReplay`) names the macro owner for boundary cases such as
   a `__VA_OPT__` activation or a generated selector.
5. **Routing.** `DispatchStructuralHunks` sends the hunk to the family planner
   for its owner (§3). Owners the narrowing ladder has ruled out
   (`ownersMustExpand`) are expanded rather than preserved.

Whether a path is in the declared domain at all is a property of its proof
summary, not of routing. `proof/RefoldCompletenessTypes` defines the contracts,
and the proof-summary builder derives them (§4).

## 3. Witness

A witness is a typed, value-only record of the evidence that justifies one
realization. Each family builds its own and names the producer fact that
discharged it, so an audit can check the fact instead of re-deriving it from
emitted text. Witness types live in the `carriers` layer:
`proof/RefoldMacroPatchTypes.h`, `proof/RefoldAnchorWitnessTypes.h`,
`proof/RefoldTilingWitnessTypes.h`, `proof/RefoldCandidateTypes.h` and
`proof/RefoldSidebandReplayProof.h`.

| Family | Builds | Owner |
| --- | --- | --- |
| Macro invocation | `MacroPatch` with a `MacroPatchProof`: args-only (standard, paste, pure paste, paired pure insertion), callee substitution, paste-derived callee selector, recursive tuple-generated-callee replay, DAG subtree root, call-chain suffix, counter literal, whole-cover realization | `macro/RefoldMacroPatchPlanner` and `macro/RefoldMacroWholeCoverOrchestrator` with its phases; certified by `macro/RefoldMacroPatchProofCertifier`; replay gates in `macro/RefoldMacroReplayStabilityValidator` and `macro/RefoldMacroSubtreeReplayValidator` |
| Include | `IncludePatch` with an include-preserving anchor, or an include realization | `include/RefoldIncludeInsertionPlanner`, `include/RefoldIncludeReplayProof`, `include/RefoldHeaderIncludeEditPlanner` |
| Translation unit | TU insertion anchors, TU byte spans, direct hunk plans | `edit/RefoldTUAnchorProof` (proofs; mints the TU-anchor candidate), `edit/RefoldTUEditPlanner` (plans) |
| Owner realization | `OwnerRealizationResult` over an `OwnerClosure` | `proof/RefoldOwnerRealizationProofBuilder`, on facts from `proof/RefoldOwnerStateProof` |
| Structural tiling | `StructuralHunkTilingWitness`, and the per-structure answers it rests on | the tiling planner; `proof/RefoldStructuralHunkTilingProof`, `proof/RefoldStructuralGapCrossingProof`, `source/RefoldSourceGapProof`, `proof/RefoldNeutralityProof` |
| Observers | line-control observer, counter state, zero-token boundary, variadic comma witnesses | attached by `RefoldOwnerRealizationProofBuilder::AttachStandardWitnesses` |
| Sideband pragmas | `SidebandPragmaEditProof` binding a source pragma edit to raw-B bytes | `sideband/RefoldSidebandPragmaEdits`, `proof/RefoldSidebandReplayProof` |

Candidates are built in a fixed preference order, which CLAUDE.md §6 lists:
direct args-only replay first, the terminal fallback last. That order decides
only which candidates are built. Every one of them still passes the same
admission (§4) and competition (§5).

## 4. Normalization

Normalization turns a family's patch or edit into one
`AcceptedResultCandidate` whose `ProofSummary` names exactly one
`TheoremProofClass`. A candidate that does not normalize cannot be selected,
and an edit without a normalized carrier cannot be emitted.

| Step | Owner |
| --- | --- |
| Map the construction path (`AcceptedPathKind`) to its theorem class, acceptance-path inventory and baseline discharge | `proof/RefoldAcceptancePathClassifier` |
| Classify a macro patch's proof carrier and run its fail-closed validators | `proof/RefoldMacroPatchProofClassifier` |
| Build the candidate for each surface (`BuildAcceptedMacroCandidate`, `…IncludeCandidate`, `…TUTextEditCandidate`, `…TerminalCandidate`, …) | `proof/RefoldAcceptedCandidateBuilder` |
| Attach proof overlays: owner realization, the matching structural tiling witness | `proof/RefoldOwnerRealizationProofBuilder` |
| Configure and finalize the summary: theorem class, realization mode, completeness and theorem-domain contracts, and the canonical `EmittedProof` | `proof/RefoldProofSummaryBuilder` |

The admission gate is `RefoldProofSummaryBuilder::NormalizeAcceptedProof`. For
a non-terminal candidate it builds the canonical `EmittedProof` only when:

- the construction path is known and backed by an explicit proof;
- the theorem class is declared explicitly, and is neither `Unknown` nor the
  terminal class;
- the discharge record is `Discharged`;
- the completeness contract covers a declared proof class, and the
  theorem-domain contract places the summary inside the declared domain;
- an owner-realization witness is present where the path requires one;
- the class-specific gate holds. For `StructuralHunkTilingProof`, for
  example, the witness must be a unique, composed partition with an exact
  segment selection.

`FinalizeProofSummary` marks a summary without an explicit primary class as
rejected. It never picks a different class for it.

Terminal failures are normalized too. `proof/RefoldTerminalProofSink` accepts a
`TerminalFallbackProofFailure`, audits it as it arrives, and maps its local
reason onto the theorem-facing `TheoremFallbackFailureKind`.

## 5. Competition

Competition chooses among candidates that all passed normalization. It is the
only place ordering policy lives.

- **Admission before preference.** `RefoldAcceptedResultRanker` asks
  `IsSelectable*` first; the preference relation is never applied to a
  candidate that failed normalization.
- **A strict order.** `ProofDominates` compares a lexicographic key built from
  the two summaries alone:
  1. `SelectionPreference` rank;
  2. `SurfaceDisposition` rank, which prefers staying closer to the original
     structure;
  3. the structural-tiling rule: a durable tiling beats the owner-specific
     summary it competes with, and of two tilings the narrower cover wins;
  4. the theorem-class enum;
  5. the construction-path enum.

  The key is irreflexive, asymmetric and transitive, so the max scan does not
  depend on push order. When proof tracing is enabled, the selector audits
  those laws on each competition (`FindSelectionOrderViolation`), and in
  strict mode a violation refuses the competition.
- **Canonical tie-break only on incomparability.** When neither summary
  dominates, `AcceptedResultCandidateCanonicalPrefers` picks by artifact shape.
  This is a representative choice, not a validity test.
- **Named tie-breakers need a proven equivalence.** A preference between two
  representations of the same edit is consulted only through
  `ProvenEquivalentArtifactPrefers`, after the caller has proved both realize
  the same edit. The engine's TU-edit versus macro args-only tie-breaker in
  `DispatchStructuralHunks` is one such caller.
- **The witness resolver.** `proof/RefoldWitnessResolver` builds a witness per
  selectable candidate, keys it with
  `proof/RefoldWitnessEquivalenceKeyBuilder`, and partitions the set into
  equivalence classes. In strict mode it can override the ranker. It picks the
  canonical maximum when there is one class, or when several classes are proved
  to describe one edit. It refuses the competition when complete, converted
  keys prove non-equivalent classes, or when composition fails fatally. When
  keys are incomplete or a proof family is not yet converted, the ranker's
  choice stands. In other modes the resolver only reports.
  `proof/RefoldWitnessClassifier` holds the vocabulary it classifies with.
- **The theorem audit.** `proof/RefoldTheoremAudit` records the resolver's
  decision and audits the winner of an accepted-result competition for legacy
  authority. After the pass,
  `EnforceTheoremAuditInvariants` turns any surviving violation into a terminal
  request.

Competitions run where candidates meet, and all of them use the ranker's
`SelectPreferred*` entry points:

- `macro/RefoldMacroFinalCandidateSelector`, among the macro candidates for one
  invocation;
- `RefoldHeaderIncludeEditPlanner::SelectBestInsertCandidate`, among include
  insertion anchors;
- the accepted-candidate builder.

## 6. Composition

Composition decides whether the chosen artifacts combine into one consistent
edit set, and repairs preprocessing state the combination would otherwise
change.

| Concern | Owner |
| --- | --- |
| Segments of one hunk compose: state summaries, owner boundaries, target token stream, B boundary projection | the tiling planner, checked by `proof/RefoldStructuralHunkTilingProof` |
| Witnesses in one selection compose into one global tuple | `RefoldWitnessResolver::ClassifyWitnessComposition` |
| Staging: coalesce macro patches by physical invocation span, order include insertions, build closure intervals | `source/RefoldStructuralHunkDispatcher` (it does not choose proof classes) |
| Each pure B insertion emitted at most once | `edit/RefoldBInsertionLedger` |
| Producer-recorded `#define`/`#undef` transitions survive the staged TU edits, preserved or materialized | `macro/RefoldMacroStateRepairPlanner`, `macro/RefoldMacroStateProof` |
| `__COUNTER__` values after an edit stay unchanged | `macro/RefoldCounterStabilization` |
| `__LINE__` and other line observers see the same line | `line-control/RefoldLineObserverLayout` (realizes demand proved by `RefoldLineControlProof`) |
| Include materialization order and seeds | `include/RefoldIncludeMaterializationScheduler` |
| `#pragma once` state survives inlining | `include/RefoldPragmaOnceGuardRewriter` |
| An unresolved hunk realized as a TU include closure | `RefoldExpansionFallbackPlanner::BuildTUIncludeClosureEditForUnresolvedHunk` |
| Capability to touch protected preprocessing structure, bound to exact physical intervals | `edit/RefoldTextEditCertifier` (`Authorize*`) |

`RefoldTextEditAssembler::AuditAcceptedEditProofs` closes composition. It is
the last gate before any byte is spliced into a file, and it rechecks the
planners' claims independently:

- it runs the certifier's global firewall,
  `RefoldTextEditCertifier::AuditGlobalSourceEditInvariant`: no edit touches
  protected structure without an exact, compatible capability, and lexical
  widening contains only trivia;
- every edit carries only discharged, normalized carriers, and multiple
  carriers on one edit form an ordered, gap-free sequence;
- every gap a tiling preserved in place is untouched by the final edit set.

## 7. Emission

| Step | Owner |
| --- | --- |
| Lower TU macro edits, stage TU-root include edits, assemble with pending line resync, repair the line-control prologue | `edit/RefoldFinalTUEmissionPlanner` |
| Splice edits into one file, deferring `#line` resync to the next safe line start | `RefoldTextEditAssembler::ApplyTextEditsWithPendingResync` |
| Realize include bodies (inline from B, header-local edits, child anchoring) | `include/RefoldIncludeMaterializer` |
| Remove a final line-control directive only when the injected oracle proves the output unchanged | `line-control/FinalLineControlModel` (pruner), oracle from `source/RefoldPreprocessRecheck` |
| Check that `preprocess(assembly)` equals `preprocess(B)` | `edit/RefoldFinalAssemblyVerifier` |
| Check preserved line observers in the final output | `RefoldEngine::AuditPreservedLineObserversInFinalOutput` |
| Normalize a failed pass onto the terminal carrier | `RefoldExpansionFallbackPlanner::ResolvePostStructuralFallback` |
| Narrow and retry, or fail | `core/RefoldRunController.cpp` |

The closing check re-preprocesses the finished assembly. It is the only check
that does not rest on the claims it polices. Inside the engine it runs only
when the final line-control prune changed the text. The controller runs it on
each attempt that did not end in a terminal request.

A proven divergence or a terminal request that names a region adds that
region's owner to `ownersMustExpand` and starts a new pass. The set only grows,
so the ladder terminates. When nothing narrower is left, the controller does
not emit the edited stream B: that would drop every comment and directive in
the file. It returns an error naming every terminal request instead
(`takeTerminalCarrier`).

`--verify-output` sets what a divergence means: `fatal` fails the run, `repair`
narrows, and `off`, the default, builds no verifier. The lit harness defaults
to `fatal`.

## Changing the pipeline

- A new theorem family adds its witness type to a `carriers` header, its
  builder to a planner or proof service, and a case to the acceptance-path
  classifier and the proof-summary builder's class gate. It does not add a
  selection rule of its own.
- A new preference goes into `ProofDominates` only if it compares two summaries
  alone and keeps the order strict. A preference that needs a caller-proved
  fact is a named tie-breaker.
- Diagnostics read state that has already been computed. They must not change
  what is selected or emitted.

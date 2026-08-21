// RUN: %clang-refold-tester-relaxed macro_state_gap_payload_naming_macro_identifier_refuses
// XFAIL: *
// Fail-closed regression: an undetermined payload that expands to a bound macro
// is not insensitive to the `#define` it straddles, even though it never spells
// that `#define`'s name.
//
// Same shape as `macro_state_gap_payload_naming_unbound_identifier_commits`,
// except that `ZZ` is itself a macro, and one whose replacement list names
// `VALUE`.  The payload spells `VALUE` nowhere, so the directive's own
// observation rule -- object-like, observed by the bare name -- reports nothing,
// and a proof built only on that rule would commit a side.
//
// Both sides are wrong.  `ZZ` is live at the replay position, so it expands
// there: after the directive it reaches `3`, before it reaches an undefined
// `VALUE`.  The two placements do not re-preprocess to the same tokens, which
// is exactly what the insensitivity theorem claims when it commits a side.
//
// The macro-state proof is what must report this, and it does:
//
//   payload identifier 'ZZ' expands to a preserved macro-state binding it does
//   not spell
//
// so the tiler refuses the split (`tiling/gap-crossing ... crossable=false
// reason=PayloadObservesState`) and one wide edit spans the `#define`.
//
// WHERE IT REFUSES.  That wide edit reads `ZZ` while `ZZ` is still bound, and
// no macro-state repair discharges it, so the closing macro-liveness audit
// rejects it: `obligation=ProducerFactsAvailable reason=NoCanonicalSuffixOrder
// stage=macro/liveness`.  It used to reach `UncomposableEmissionEditSet` at
// `emit/nonterminal` instead; the audit added by
// `macro_state_payload_names_live_macro_no_repair_refuses` now catches the same
// input one stage earlier and names the actual obligation.
//
// TRIAGE: incompleteness, narrowly.  No placement of the payload alone realizes
// B, but the `#undef ZZ` the liveness planner already knows how to synthesize
// does -- `ZZ` is used nowhere after the array, so undefining it is
// unobservable.  The expected refold below is that repair.
//
// TWO BLOCKERS, both measured by disabling the reachability walk in
// `PayloadObservesMacroStateBindings` and rerunning this input:
//
//   1. The tiler must admit the crossing.  Under the state the refold is
//      *required* to establish -- `ZZ` inert, because B already expanded it --
//      the payload cannot reach `VALUE` at all, so the placement question and
//      the ZZ-neutralisation question are independent and the reachability walk
//      conflates them.  Relaxing it wholesale is not sound: the walk also
//      guards `RefoldExpansionFallbackPlanner`'s placements, whose TU include
//      closure edits the macro-liveness audit deliberately does not cover
//      (mixed B/preserved-source replacements, the standing partition gap).
//      Any relaxation has to be scoped to callers whose resulting edits the
//      audit does police.
//
//   2. Emission must compose the repaired edit set.  With the walk disabled the
//      tiler does split, and the `#undef` synthesis does fire
//      (`macro/liveness: synthesizing local #undef partition`), but the refold
//      then fails with `UncomposableEmissionEditSet`, detail "pre-normalization
//      edit set does not contain exactly the structural token segments for the
//      current source owner".  `RestageConservativeTUEdit` ->
//      `PromoteToSpecializedMacroStateRepairCarrier` erases the ordinary
//      direct-TU carrier, and that carrier is what held
//      `structuralSegmentIndex`.  The repaired edit still realizes the same
//      structural segment; the binding is simply dropped, and the emission
//      audit correctly notices it is gone.
//
// The original comment predicted exactly this second failure, and it
// reproduces.  What must not happen is reaching the fold by letting the
// insensitivity theorem commit a side.
#define ZZ VALUE
int arr[] = {
#define VALUE 3
#undef ZZ
ZZ };

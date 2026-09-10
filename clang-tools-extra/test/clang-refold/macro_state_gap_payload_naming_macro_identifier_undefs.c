// RUN: %clang-refold-tester-relaxed macro_state_gap_payload_naming_macro_identifier_undefs
// Regression: a payload that expands to a bound macro is neutralised by a
// synthesized `#undef`, not by committing a side of the directive it straddles.
//
// Same shape as `macro_state_gap_payload_naming_unbound_identifier_commits`,
// except that `ZZ` is itself a macro, and one whose replacement list names
// `VALUE`.  The payload spells `VALUE` nowhere, so the directive's own
// observation rule -- object-like, observed by the bare name -- reports
// nothing, and a proof built only on that rule would commit a side.
//
// Committing either side is wrong.  `ZZ` is live at the replay position, so it
// would expand there: after the directive it reaches `3`, before it an
// undefined `VALUE`.  The two placements do not re-preprocess to the same
// tokens, which is exactly what the insensitivity theorem claims when it
// commits a side.
//
// The answer is not to pick a side but to remove the premise.  B is already
// preprocessed, so the `ZZ` in it is an identifier and the refold must keep it
// one; the liveness planner synthesizes `#undef ZZ` before the replacement, and
// with `ZZ` inert the payload cannot reach `VALUE` from either side.  `ZZ` is
// used nowhere after the array, so no restore is needed.
//
// Reaching that took two changes, one at each end of the pipeline.
//
//   1. The tiler was refusing the crossing.  Its reachability walk asked
//      whether any payload identifier *expands* to the preserved binding, and
//      answered yes through `ZZ` -- under a macro state the refold is obliged
//      to eliminate.  That question belongs to the macro-liveness audit, which
//      walks every TU edit whose replacement is wholly mapped B payload and
//      requires each live name it reads to be unbound where it lands.  So the
//      leg is now deferred to that audit, but only for callers in its domain:
//      `RefoldExpansionFallbackPlanner`'s TU include closures mix B payload
//      with preserved source, where a name may be an expansion the source
//      requires, and those keep answering the question themselves.
//
//   2. Emission was dropping the repaired edit's structural-segment binding.
//      Promoting an edit to the specialized macro-state repair carrier removes
//      the direct-TU carrier, correctly -- the repair invalidates its byte-span
//      theorem -- but that carrier also named which segment of the structural
//      tiling the edit realizes, which the repair does not change.  Losing it
//      made the edit vanish from the emission census and the assembly was
//      refused as uncomposable.  The key is now inherited across the promotion
//      and re-resolved through the durable planner ledger.
#define ZZ VALUE
int arr[] = { 1,
#define VALUE 3
2 };

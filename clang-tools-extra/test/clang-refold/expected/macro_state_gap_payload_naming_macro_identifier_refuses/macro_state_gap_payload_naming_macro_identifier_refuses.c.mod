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
// That distinction matters and is not visible in the exit status.  With the
// reachability walk disabled the tiler commits a side, the liveness planner
// synthesizes an `#undef ZZ` before the payload, and emission fails to compose
// the resulting edit set -- so the refold still fails, for an unrelated
// downstream reason.  A check on failure alone would keep passing while the
// test stopped testing its name, so the evidence string above is recorded here
// deliberately: an XFAIL cannot assert it, since a RUN line placed after a
// failing one never runs.
//
// TRIAGE: incompleteness, narrowly.  No placement of the payload alone realizes
// B, but the `#undef ZZ` the liveness planner already knows how to synthesize
// does -- `ZZ` is used nowhere after the array, so undefining it is
// unobservable.  The expected refold below is that repair, and it is the fold
// this cell should eventually produce.  What must not happen is reaching it by
// letting the insensitivity theorem commit a side.
#define ZZ VALUE
int arr[] = {
#define VALUE 3
#undef ZZ
ZZ };

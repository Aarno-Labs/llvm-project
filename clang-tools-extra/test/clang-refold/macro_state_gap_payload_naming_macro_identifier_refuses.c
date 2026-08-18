// RUN: %clang-refold-tester-relaxed-expect-refold-fail macro_state_gap_payload_naming_macro_identifier_refuses
// RUN: FileCheck %s --check-prefix=REACH < %t/outputs/macro_state_gap_payload_naming_macro_identifier_refuses.out
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
// PINS A REFUSAL AT ONE STAGE, NOT MERELY AN EXIT CODE.  Without the
// reachability walk the tiler commits a side, the liveness planner then
// synthesizes an `#undef ZZ` before the payload, and emission fails to compose
// the resulting edit set -- so the refold still fails, for an unrelated
// downstream reason, and a test asserting only on failure would keep passing
// while ceasing to test its name.  The FileCheck line above asserts that the
// macro-state proof is what reported the payload observing.
//
// REACH: payload identifier 'ZZ' expands to a preserved macro-state binding it does not spell
#define ZZ VALUE
int arr[] = { 1,
#define VALUE 3
2 };

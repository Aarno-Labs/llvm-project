// RUN: %clang-refold-tester-expect-refold-fail structural_gap_straddle_unknown_directive_refuses
// Refusal shape: one B token replacing material on both sides of a gap
// holding an unrecognized directive.
//
// An unknown directive is a hard error outside a skipped arm, so the only
// reachable form of this cell wraps it in `#if 0`, and the gap therefore holds
// three structures rather than one.  It still refuses at the same place:
// `obligation=OwnerClosedCover reason=NoOwnerClosedCover stage=classify`.
//
// PINNED BY ASSERTION, NOT BY `XFAIL`.  The harness requires the refold to
// fail, so this reports as a passing test.  A shape that must never fold is not
// unfinished work, and an expected-failure entry would say it is -- and would
// sit in the remaining-work count forever.  If the shape ever starts folding
// this test fails, which is the same alarm `XPASS` used to raise.  The expected
// refold beside this file is no longer diffed; it stays as the record of what
// admitting it would have produced.  The `.c.i` still is, so producer drift is
// still caught.
//
// TRIAGE: unclassified by design.  An unrecognized directive may do anything,
// so refusing to move a payload across one is the fail-closed default firing as
// intended.  The expected refold below records what admitting it would have to
// produce; it should stay expected-to-fail unless a proof appears that a
// skipped arm's contents cannot be observed at all.
int arr[] = { 1,
#if 0
#nonstandard directive
#endif
2 };

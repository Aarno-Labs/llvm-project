// RUN: %clang-refold-tester structural_gap_straddle_unknown_directive_refuses
// XFAIL: *
// Refusal shape: one B token replacing material on both sides of a gap
// holding an unrecognized directive.
//
// An unknown directive is a hard error outside a skipped arm, so the only
// reachable form of this cell wraps it in `#if 0`, and the gap therefore holds
// three structures rather than one.  It still refuses at the same place:
// `obligation=OwnerClosedCover reason=NoOwnerClosedCover stage=classify`.
//
// TRIAGE: unclassified by design.  An unrecognized directive may do anything,
// so refusing to move a payload across one is the fail-closed default firing as
// intended.  The expected refold below records what admitting it would have to
// produce; it should stay expected-to-fail unless a proof appears that a
// skipped arm's contents cannot be observed at all.
int arr[] = {
#if 0
#nonstandard directive
#endif
9 };

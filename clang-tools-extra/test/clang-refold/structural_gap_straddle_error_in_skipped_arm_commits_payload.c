// RUN: %clang-refold-tester structural_gap_straddle_error_in_skipped_arm_commits_payload
// Regression: a gap holding `#if 0` / `#error` / `#endif` is crossed.
//
// The gap holds three structures, the conditional controls are admitted by the
// arm-selection rule, and the crossing is decided structure by structure.  The
// middle directive is a `#error` the producer recorded inside an arm it did
// not select, so it never executed and preserving the gap's bytes in place
// leaves it unexecuted.  The same rule answers an unrecognized directive in
// that position (`structural_gap_straddle_skipped_unknown_directive_commits_payload`).
//
// A reached `#error` is deliberately not admitted.  It has already made the
// translation unit ill-formed, and nothing available here separates "reached"
// from "skipped" except the producer's arm selection, which is what this proof
// reads.
int arr[] = { 1,
#if 0
#error boom
#endif
2 };

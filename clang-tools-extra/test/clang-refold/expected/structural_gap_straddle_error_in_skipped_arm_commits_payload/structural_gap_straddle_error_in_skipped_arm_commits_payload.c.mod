// RUN: %clang-refold-tester structural_gap_straddle_error_in_skipped_arm_commits_payload
// Regression: a gap holding `#if 0` / `#error` / `#endif` is crossed, and the
// contrast with `structural_gap_straddle_unknown_directive_refuses` is that
// this file's middle directive is one the engine classifies.
//
// The two inputs differ in exactly one line.  Both gaps hold three structures,
// both surround it with a conditional control the arm-selection rule admits,
// and in both the crossing is decided structure by structure.  Here the middle
// directive is a `#error` the producer recorded inside an arm it did not
// select, so it never executed and preserving the gap's bytes in place leaves
// it unexecuted; there the middle directive is unrecognized, may do anything,
// and takes the whole gap down with it.
//
// A reached `#error` is deliberately not admitted.  It has already made the
// translation unit ill-formed, and nothing available here separates "reached"
// from "skipped" except the producer's arm selection, which is what this proof
// reads.
int arr[] = { 
#if 0
#error boom
#endif
9 };

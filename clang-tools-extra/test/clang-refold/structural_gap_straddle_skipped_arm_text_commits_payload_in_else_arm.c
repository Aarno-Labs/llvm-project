// RUN: %clang-refold-tester-verify-off structural_gap_straddle_skipped_arm_text_commits_payload_in_else_arm
// Regression: a gap holding a skipped arm's ordinary text is crossed.
//
// The payload `9` replaces `1,` and `2`, which sit on opposite sides of
// `#ifdef` / `3,` / `#else`, so structural tiling must split the hunk at
// that gap.  The directives are indexed structure, but `3,` is neither a
// directive nor lexer trivia: it is text the preprocessor skipped.  The gap
// proof accepts it only because the producer recorded, from Clang's own
// `SourceRangeSkipped` callback, that this range of this file instance was
// skipped -- so it produced no token and changed no state, and preserving it
// in place leaves it skipped.  The arm-selection flag on the arm record is not
// that fact and is not consulted for it.
//
// With the gap proven, the crossing is the ordinary conditional-control one
// (`proof=ConditionalControlSelectedArm`): the payload lands inside the
// `#else` arm the preprocessor took.  Before the skipped ranges were recorded
// the tiler found no partition and the whole translation unit was refused.
int arr[] = { 1,
#ifdef REFOLD_TEST_UNDEFINED_ARM
3,
#else
2 };
#endif

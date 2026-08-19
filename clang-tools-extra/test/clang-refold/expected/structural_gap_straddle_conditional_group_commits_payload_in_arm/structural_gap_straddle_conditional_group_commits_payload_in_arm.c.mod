// RUN: %clang-refold-tester structural_gap_straddle_conditional_group_commits_payload_in_arm
// Regression: one B token replacing material on both sides of a conditional
// control is committed inside the arm the control opens.
//
// The payload's side is not fixed by alignment -- it replaces `1,` and `2`,
// which sit on opposite sides of the `#if` -- so the tiler must prove the two
// placements equivalent before it may choose one.
//
// The control directive itself stays exactly where it was, so what the choice
// decides is only whether the payload lands before the group or inside the arm.
// The run carrying the tokens after the gap contributed A tokens, and the
// producer records the arm those tokens belong to as selected, so the payload's
// new home is reached exactly as its old one was.  The gap-crossing proof
// reports `proof=ConditionalControlSelectedArm`.
//
// The same theorem covers `#ifdef`, `#ifndef`, `#endif` and a whole
// `#elif`/`#else` group in the gap; only one is pinned here.
int arr[] = { 
#if 1
9 };
#endif

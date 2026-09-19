// RUN: %clang-refold-tester-verify-off surviving_pragma_straddle_commits_payload_before_printed_directive
// Mirror of `surviving_pragma_straddle_commits_payload_after_printed_directive`:
// B prints the re-emitted pragma after the payload instead of before it, so the
// payload has to be committed ahead of the preserved directive.
//
// Alignment admits the same two boundaries, B=6 and B=7, at the seam.  B's
// `#pragma pack(1)` now sits after `9`, so B=7 is the only one that reproduces
// B's token stream.  A planner that commits one fixed side -- as the consumed
// pragma crossing does, where both sides are equivalent -- passes one of these
// two tests and fails the other.
//
// Verification is off so the planner, not the closing check, has to get the
// side right.
int arr[] = { 1,
#pragma pack(1)
2 };

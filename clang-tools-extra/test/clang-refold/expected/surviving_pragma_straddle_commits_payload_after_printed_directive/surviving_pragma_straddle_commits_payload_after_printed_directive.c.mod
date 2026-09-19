// RUN: %clang-refold-tester-verify-off surviving_pragma_straddle_commits_payload_after_printed_directive
// One B token replacing material on both sides of a pragma that clang
// re-emits takes the side B prints the pragma on.
//
// The payload replaces `1 , 2`, which spans the directive, so alignment alone
// leaves the seam undetermined:
//
//   possible B frontiers protectedA=8 frontiers=[6,7]
//
// But `pack` is spelled into both streams, and the directive stays in source,
// so the refolded output prints it exactly at the seam.  B prints it before
// `9`, which makes B=6 the only boundary that reproduces B's token stream --
// the other one fails `--check` with `A='9' B='#'`.  The side is B's token
// order, not a choice:
//
//   seam A=8 undetermined in B=[6,7] is fixed at B=6 by the 1 preserved
//   pragma(s) B prints there
//
// Nothing about what `pack` means is assumed.  Every pragma that changes macro
// state is one clang consumes, so a gap holding only printed pragmas cannot
// change the payload's expansion; what `pack` binds to at compile time follows
// the order B already fixed.
//
// This used to refuse the whole translation unit as
// `obligation=OwnerClosedCover reason=NoOwnerClosedCover`, on the claim that
// both placements replay B.  Only one does.  The mirror case is
// `surviving_pragma_straddle_commits_payload_before_printed_directive`, which is
// what keeps this from passing by always committing one side.
//
// Verification is off so the planner, not the closing check, has to get the
// side right.
int arr[] = { 
#pragma pack(1)
9 };

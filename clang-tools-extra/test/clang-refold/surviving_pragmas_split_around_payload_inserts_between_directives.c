// RUN: %clang-refold-tester-verify-off surviving_pragmas_split_around_payload_inserts_between_directives
// A replacement whose payload B prints between two re-emitted pragmas of one
// source gap lands between the directive lines.
//
// The payload replaces `1 , 2`, which spans both directives.  Each directive
// survives into B and names a different side of the payload -- B=6 for
// `pack(1)`, B=7 for `pack(2)` -- so no single seam boundary reproduces B and
// a structural tiling, which keeps the gap whole, cannot express it.  B's
// token order still fixes everything:
//
//   seam A=8 of A=[6,9) B=[6,7): B prints its preserved pragmas at B=6 and
//   B=7; splitting the payload between them into an insertion at the seam
//
// `1 ,` and `2` are deleted in place, and `9` is inserted at the start of the
// `pack(2)` line, the one directive B prints after it.  Both pragmas are
// printed, not consumed, so nothing between them can change what `9` means;
// only the order of the three lines is at stake, and B fixes it.
//
// This used to refuse the whole translation unit as
// `obligation=OwnerClosedCover reason=NoOwnerClosedCover`.
//
// Verification is off so the planner, not the closing check, has to put the
// payload between the directives -- the placement on either side of both
// fails `--check`.
int arr[] = { 1,
#pragma pack(1)
#pragma pack(2)
2 };

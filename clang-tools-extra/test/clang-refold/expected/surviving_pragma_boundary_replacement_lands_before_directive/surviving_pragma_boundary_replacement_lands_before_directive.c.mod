// RUN: %clang-refold-tester-verify-off surviving_pragma_boundary_replacement_lands_before_directive
// A replacement that starts at a surviving pragma's gap, but that B prints
// before the pragma, is realized as an insertion before the directive and a
// deletion after it.
//
// `+ 2` follows `#pragma pack(1)` in source; B replaces it with `- 3` printed
// *before* the pragma.  The token hunk A=[4,6) B=[4,6) starts at the
// directive's A gap, so realizing it in place prints the preserved directive
// before `- 3`, not after.  Pairing records that B prints the directive at
// B gap 6, and B's token order forces the payload across it:
//
//   A gap 4 holds surviving directive(s) B prints at B=[6,6] but the hunks
//   print at B=[4,4]; moving B=[4,6) into a pure insertion at the gap
//
// `- 3` is then inserted at the start of the directive's line, and `+ 2` is
// deleted after it.
//
// This used to exit zero at the default `--verify-output=off` with wrong
// output: `#pragma pack(1)` printed before `- 3`.
int x = 1
 - 3
#pragma pack(1)
;

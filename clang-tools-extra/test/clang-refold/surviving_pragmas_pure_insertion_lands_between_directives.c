// RUN: %clang-refold-tester-verify-off surviving_pragmas_pure_insertion_lands_between_directives
// A pure insertion that B prints between two re-emitted pragmas lands between
// the directive lines, and does not replay the directive B prints after it.
//
// The insertion `- 22` sits at A gap 4, which holds both preserved
// directives.  B prints `pack(1)` before it and `pack(2)` after it.  The
// insertion's token envelope runs up to the next token, so it also spans B's
// copy of `pack(2)` -- a line the preserved source directive already prints.
// The anchor is therefore placed at the `pack(2)` line and the replay cut
// before B's copy of it:
//
//   insertion A=4 B=[4,6) placed at source byte ... on the side of each
//   preserved pragma B prints it on
//
// This used to exit zero at the default `--verify-output=off` with wrong
// output: the envelope and its trailing `#pragma pack(2)` were inserted after
// the source `pack(2)`, printing that directive twice and on the wrong side of
// `- 22`.
int a = 1
#pragma pack(1)
#pragma pack(2)
+ 3;

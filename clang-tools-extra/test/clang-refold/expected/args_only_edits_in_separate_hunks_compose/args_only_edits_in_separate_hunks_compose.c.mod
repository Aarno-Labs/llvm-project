// RUN: %clang-refold-tester-verify-off args_only_edits_in_separate_hunks_compose
// Rewrites of different formals planned for separate hunks of one invocation
// still compose.
//
// B renames `b` and `c` consistently, in separate hunks.  The rewrite staged
// for the first hunks changes only `b`, so its re-expansion keeps `int r`;
// that token belongs to a hunk not yet planned, and the check that a slot's
// patch reproduces B leaves such tokens open rather than refuting the partial
// rewrite.  The second rewrite merges with it into `R(y, q)`.
#define R(b, c) char *s = #b; int b; int c; char *t = #c;
R(y, q)

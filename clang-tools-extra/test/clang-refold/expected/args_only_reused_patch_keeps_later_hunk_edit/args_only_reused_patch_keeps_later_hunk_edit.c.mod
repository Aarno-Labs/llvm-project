// RUN: %clang-refold-tester-verify-off args_only_reused_patch_keeps_later_hunk_edit
// A patch staged for an earlier hunk of an invocation may not be reused for a
// later hunk it does not reproduce.
//
// The first hunks rename `c` everywhere and stage `R(x, q)`.  The last hunk
// renames `int x` but keeps `#b`'s "x", which no rewrite of `b` explains; the
// selector used to reuse the staged `R(x, q)` for it, silently dropping
// `int y`.  That rewrite keeps `int x`, which B cannot hold, so the
// invocation is realized from B instead.  This used to emit `R(x, q)` at
// verify-off.
#define R(b, c) int c; char *t = #c; char *s = #b; int b;
int q; char *t = "q"; char *s = "x"; int y;

// RUN: %clang-refold-tester-verify-off args_only_later_hunk_keeps_earlier_realized_edit
// A later hunk's rewrite of an invocation may not replace the realization an
// earlier hunk of the same invocation chose unless it reproduces that hunk too.
//
// B renames `int x` but keeps `#b`'s "x", so no rewrite of `b` explains the
// first hunk and the invocation is realized from B.  The second hunk renames
// `c` everywhere, and `R(x, q)` explains it alone; the selector used to prefer
// that structure-preserving rewrite and replace the realization, silently
// dropping `int y`.  Its re-expansion keeps `int x`, which B cannot hold, so
// the realization stays.  This used to emit `R(x, q)` at verify-off.
#define R(b, c) char *s = #b; int b; int c; char *t = #c;
char *s = "x"; int y; int q; char *t = "q";

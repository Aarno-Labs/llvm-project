// RUN: %clang-refold-tester-verify-off args_only_spliced_formal_reference_keeps_pasted_observer
// An args-only rewrite is refused when a generated call names a caller formal
// across a line splice.
//
// G's actual `a\⏎b` is C's formal `ab`, but its raw spelling is not `ab`.  The
// producer used to compare raw spellings with C's formal names and record
// `arg_deps: [[]]`, which told the descendant gate that G's pasted `g_x`
// ignores `ab`.  B renames `int x` and the root's own `#ab`; rewriting the
// actual to `y` would also turn the unchanged `g_x` into `g_y`.  The fold
// fails closed and the invocation is expanded.  This used to emit `C(y)` at
// verify-off.
#define G(v) int g_##v;
#define C(ab) G(a\
b) int ab; char *s = #ab;
C(x)

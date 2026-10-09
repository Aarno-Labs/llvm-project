// RUN: %clang-refold-tester-verify-off args_only_generated_call_closed_by_actual_keeps_pasted_observer
// An args-only rewrite is refused when an unchanged token comes from a
// generated call whose name and argument list are spelled in different places.
//
// `G P` names G in C's replacement list and takes `(x)` from C's actual.  The
// text the producer records for the call therefore runs from the #define to
// the callsite, and it used to parse `void` out of `int f(void);` as G's
// argument and record `arg_deps: [[]]`, which told the descendant gate that
// G's pasted `g_x` ignores P.  B changes only the root's own `#P`; rewriting P
// to `(y)` would also turn the unchanged `g_x` into `g_y`.  The fold fails
// closed and the invocation is expanded.  This used to emit `C((y))` at
// verify-off.
#define G(v) int g_##v;
#define C(P) G P char *s = #P;
int f(void);
C((x))

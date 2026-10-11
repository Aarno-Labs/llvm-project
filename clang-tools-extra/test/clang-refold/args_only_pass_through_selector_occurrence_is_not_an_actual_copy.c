// RUN: %clang-refold-tester-verify-off args_only_pass_through_selector_occurrence_is_not_an_actual_copy
// An occurrence of a formal that holds the output of a callee it selects is
// not a copy of the actual, even when that callee only passes its argument
// through.
//
// `f` names `F` in `f(G(a))`, and `F` returns its argument, `G`'s `g_x`.  The
// producer credits `F` with no token, since its whole output is an argument,
// and attributes `g_x` to `f` through `F`'s name.  The check that a selected
// callee's output is not a copy looked only at tokens credited to the callee,
// so it saw nothing.  B spells `h` there; reading that as the new actual
// emitted `CALL(h, x);`, which expands to `int h(g_x);` instead of `int h;`.
// The fold is refused and the invocation is expanded.  The `;` is spelled
// after the callsite so that no unchanged token of the cover follows `F`'s
// output, which leaves this check alone to refuse.
#define F(v) v
#define G(v) g_##v
#define CALL(f, a) int a; int f(G(a))
CALL(F, x);

// RUN: %clang-refold-tester-verify-off args_only_prescanned_selector_keeps_pass_through_output
// An args-only rewrite of a formal that selects a callee is refused when the
// callee is formed while another macro prescans an actual naming two formals.
//
// `m` names `F` in `EVAL(m(v))`, and `F(x)` expands while `EVAL` prescans its
// actual, so `DISPATCH`, not the immediate caller `EVAL`, substituted the name
// `F`.  `F` returns its argument and is credited with no token, so only its
// selection ties `x` to `m`.  That selection is counted: the producer's
// `callee_subst` history of `F` names `DISPATCH`'s `m`, where `EVAL`'s text
// `arg_deps` would name both `m` and `v`.  B renames only the copy `int F;`;
// rewriting `m` to `H` also turns `int x;` into `int H(x);`.  This used to
// emit `DISPATCH(H, x)` at verify-off.  The fold is refused and the
// invocation is expanded.
#define F(v) v
#define EVAL(x) x
#define DISPATCH(m, v) int EVAL(m(v)); int m;
int x; int H;

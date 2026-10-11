// RUN: %clang-refold-tester-verify-off args_only_rewritten_selector_keeps_pass_through_output
// An args-only rewrite of a formal that selects a callee is refused when an
// unchanged token is that callee's output, even when the callee only passes
// its argument through.
//
// `f` names `F` in `f(a)` and is copied into `int f;`.  `F` returns its
// argument, so the producer credits `F` with no token and attributes `x` to
// `a` alone; nothing recorded that `x` exists only because `f` selected `F`.
// B renames only the copy, `int F;`; rewriting `f` to `H` also turns
// `int x;` into `int H(x);`.  This used to emit `CALL(H, x)` at verify-off.
// The fold is refused and the invocation is expanded.
#define F(v) v
#define CALL(f, a) int f(a); int f;
CALL(F, x)

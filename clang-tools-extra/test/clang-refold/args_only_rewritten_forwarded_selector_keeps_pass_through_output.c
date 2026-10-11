// RUN: %clang-refold-tester-verify-off args_only_rewritten_forwarded_selector_keeps_pass_through_output
// The rule of args_only_rewritten_selector_keeps_pass_through_output holds
// when the selecting formal reaches the callee through a forwarding macro.
//
// `CALL` forwards `f` to `W`, whose `f` names `F` in `f(a)`.  `F` passes `x`
// through and is credited with no token.  The selection composes through
// `W`'s actual `f`, which spells exactly `CALL`'s formal `f`, so rewriting it
// to `H` is known to reselect `F`.  B renames only the copy, `int F;`.  This
// used to emit `CALL(H, x)`, which expands to `int H(x); int H;`.  The fold
// is refused and the invocation is expanded.
#define F(v) v
#define W(f, a) int f(a); int f;
#define CALL(f, a) W(f, a)
CALL(F, x)

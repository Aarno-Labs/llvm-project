// RUN: %clang-refold-tester-verify-off args_only_opaque_selector_keeps_pass_through_output
// An args-only rewrite of a formal that selects a callee is refused when the
// callee name passes through a generated caller.
//
// `g` reaches `PASS(a)` through `APPLY` and then `FWD`, whose own name comes
// from `APPLY`'s `f`.  `FWD` has no callsite text to locate the name in, so
// `PASS`'s callee origin is opaque.  `PASS` returns its argument and is
// credited with no token, so only its selection ties `x` to `g`.  That
// selection is counted: the producer's `callee_subst` history of `PASS`
// records `FWD`'s `g`, `APPLY`'s `g` and `CALL`'s `g`.  B renames only the
// copy `int PASS;`; rewriting `g` to `H` also turns `int x;` into
// `int H(x);`.  This used to emit `CALL(H, x)` at verify-off.  The fold is
// refused and the invocation is expanded.
#define PASS(v) v
#define FWD(g, a) g(a)
#define APPLY(f, g, a) f(g, a)
#define CALL(g, a) int APPLY(FWD, g, a); int g;
int x; int H;

// RUN: %clang-refold-tester-verify-off args_only_pasted_selector_keeps_pass_through_output
// An args-only rewrite of a formal that selects a callee by pasting is
// refused when an unchanged token is that callee's output.
//
// `p##_` forms the callee name `F_` from `p`, and `F_` returns its argument,
// so the producer credits `F_` with no token and attributes `x` to `a` alone.
// The selection is counted for every descendant credited with no token, here
// through the pasted name's root slice.  B renames only the copy `int F;`;
// rewriting `p` to `H` also turns `int x;` into `int H_(x);`.  This used to
// emit `CALLP(H, x)` at verify-off.  The fold is refused and the invocation
// is expanded.
#define F_(v) v
#define CALLP(p, a) int p##_(a); int p;
CALLP(F, x)

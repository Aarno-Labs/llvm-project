// RUN: %clang-refold-tester-verify-off macro_state_undef_before_args_only_actual_called_by_body
// Regression: B replaces the actual `G` with `F`, and `M`'s own body supplies
// the `(2)` that follows it, so the rewritten `M(F)` called the live macro `F`
// during the rescan and `k` returned `2`.  B's tokens show `(` after the
// payload's `F`, so the `#undef F` partition keeps it the function.
int F(int v);
int G(int v);
#define F(v) v
#define M(a) int k(void) { return a(2); }
#undef F
M(F)

// RUN: %clang-refold-tester-verify-off macro_state_undef_before_args_only_actual_opening_call
// Regression: B calls the function `F` inside the actual of `M`.  The
// args-only rewrite `M(F (2) != 0)` kept the callsite, so `F (` was
// preprocessed as a call of the live function-like macro `F` and `k` returned
// `2 != 0`.  The patch now certifies the B tokens its rewritten actual spells,
// so the synthetic `#undef F` partition repairs it as it does a plain
// translation-unit edit, restoring `F` for the later `F(5)`.
int F(int v);
#define F(v) v
#define M(a) int k(void) { return a; }
M(F != 0)
int j = F(5);

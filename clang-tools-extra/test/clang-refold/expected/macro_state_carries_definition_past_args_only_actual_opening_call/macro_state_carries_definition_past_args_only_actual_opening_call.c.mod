// RUN: %clang-refold-tester-verify-off macro_state_carries_definition_past_args_only_actual_opening_call
// Regression: as in the call opened across actuals, but `#define F` sits
// right before the callsite, so carrying it past the rewritten `M(F, (2) != 0)`
// is preferred to synthesizing an `#undef`.  The carry must see the read the
// callsite text cannot show; it now asks B's tokens, as the partition does.
int F(int v);
#define M(a, b) int k(void) { return a b; }
M(F, (2) != 0)
#define F(v) v

int j = F(5);

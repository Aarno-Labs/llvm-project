// RUN: %clang-refold-tester-verify-off macro_state_undef_before_args_only_actual_opening_call_across_actuals
// Regression: B's `(2)` opens a call of the identifier `F`, but `F` is the
// whole first actual and `(2)` begins the second, so the callsite text never
// shows `F (`.  The rewritten `M(F, (2) != 0)` formed the call during the
// rescan of `M`'s body.  Reads are now judged on B's tokens, where `(` follows
// `F`, and the `#undef F` partition keeps `F` an identifier.
int F(int v);
#define F(v) v
#define M(a, b) int k(void) { return a b; }
#undef F
M(F, (2) != 0)

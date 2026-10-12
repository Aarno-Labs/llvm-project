// RUN: %clang-refold-tester-verify-off macro_state_expands_args_only_callsite_respelling_expanded_name
// Regression: in A the actual `X` expanded to `X + 1`, its own name left as
// the variable.  The args-only rewrite spelled B's `X + 2` into the actual,
// where `X` expanded again.  That `X` is matched to an A token, but it lands on
// an occurrence of the rewritten actual, so it is certified payload; undefining
// it over the callsite is refused, and `M` is expanded instead.
int X = 1;
#define X X + 1
#define M(a) int k(void) { return a; }
M(X)

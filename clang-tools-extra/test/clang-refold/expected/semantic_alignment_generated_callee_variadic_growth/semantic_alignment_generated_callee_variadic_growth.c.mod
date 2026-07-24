// RUN: %clang-refold-tester semantic_alignment_generated_callee_variadic_growth
// Regression: variadic forwarding must preserve the caller form when an
// ordinary argument grows across repeated parenthesis and '+' boundaries.
int prefix_marker = 17;
#define INVOKE_VARIADIC(F, X) F(X)
#define RELAY_VARIADIC(F, ...) INVOKE_VARIADIC(F, __VA_ARGS__)
#define OUTER_VARIADIC(F, ...) RELAY_VARIADIC(F, __VA_ARGS__)
#define BUMP_VARIADIC(x) ((x) + 1)
int compute_variadic(void) {
  int value = OUTER_VARIADIC(BUMP_VARIADIC, ((1) + 2) + 3);
  return value;
}
int suffix_marker = 23;

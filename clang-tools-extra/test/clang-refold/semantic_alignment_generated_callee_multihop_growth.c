// RUN: %clang-refold-tester semantic_alignment_generated_callee_multihop_growth
// Regression: semantic alignment must preserve a generated callee through
// several forwarding layers rather than expanding the complete invocation.
int prefix_marker = 17;
#define INVOKE_BASE(F, X) F(X)
#define RELAY_LEVEL1(F, X) INVOKE_BASE(F, X)
#define RELAY_LEVEL2(F, X) RELAY_LEVEL1(F, X)
#define BUMP_MULTI(x) ((x) + 1)
int compute_multi(void) {
  int value = RELAY_LEVEL2(BUMP_MULTI, 1);
  return value;
}
int suffix_marker = 23;

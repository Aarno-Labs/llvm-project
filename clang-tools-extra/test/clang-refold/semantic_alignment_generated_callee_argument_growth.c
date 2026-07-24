// RUN: %clang-refold-tester semantic_alignment_generated_callee_argument_growth
// Regression: an ordinary argument edit must preserve the outer generated-
// callee invocation when all complete optimal maps realize the same source.
int prefix_marker = 17;
#define INVOKE_ONE(F, X) F(X)
#define RELAY_ONE(F, X) INVOKE_ONE(F, X)
#define BUMP_ONE(x) ((x) + 1)
int compute_one(void) {
  int value = RELAY_ONE(BUMP_ONE, 1);
  return value;
}
int suffix_marker = 23;

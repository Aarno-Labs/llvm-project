// RUN: %clang-refold-tester macro_callee_arguments_multihop FIRST
// RUN: %clang-refold-tester macro_callee_arguments_multihop SECOND
// RUN: %clang-refold-tester macro_callee_arguments_multihop THIRD
// RUN: %clang-refold-tester macro_callee_arguments_multihop EXPAND
// Test: macro_callee_arguments_multihop
// Refold intent: exercise multi-hop macro-formal callees across several forwarding layers

#define APPLY0(F, X) F(X)
#define APPLY1(F, X) APPLY0(F, X)
#define APPLY2(F, X) APPLY1(F, X)
#define INC(x) ((x) + 1)
int main(void) {
  int v = APPLY2(INC, 3);
  return v == (1 + 1) ? 0 : 1;
}

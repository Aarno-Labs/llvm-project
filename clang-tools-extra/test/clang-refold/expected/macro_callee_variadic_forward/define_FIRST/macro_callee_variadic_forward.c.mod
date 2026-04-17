// RUN: %clang-refold-tester macro_callee_variadic_forward FIRST
// RUN: %clang-refold-tester macro_callee_variadic_forward SECOND
// RUN: %clang-refold-tester macro_callee_variadic_forward THIRD
// RUN: %clang-refold-tester macro_callee_variadic_forward EXPAND
// Test: macro_callee_variadic_forward
// Refold intent: exercise macro formals used as callees through variadic forwarding wrappers

#define APPLY(F, X) F(X)
#define FORWARD(F, ...) APPLY(F, __VA_ARGS__)
#define OUTER(F, ...) FORWARD(F, __VA_ARGS__)
#define INC(x) ((x) + 1)
int main(void) {
  int v = OUTER(INC, 3);
  return v == (1 + 1) ? 0 : 1;
}

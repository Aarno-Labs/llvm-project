// RUN: %clang-refold-tester macro_callee_arguments FIRST
// RUN: %clang-refold-tester macro_callee_arguments SECOND
// RUN: %clang-refold-tester macro_callee_arguments THIRD
// RUN: %clang-refold-tester macro_callee_arguments EXPAND
// Test: macro_callee_arguments
// Refold intent: exercise macro formals used as callees, including cases that may require fallback expansion

#define APPLY(F, X) F(X)
#define WRAP(F, X) APPLY(F, X)
#define INC(x) ((x) + 1)
int main(void) {
  int v = WRAP(INC, ((1) + 2) + 3);
  return v == (1 + 1) ? 0 : 1;
}

// RUN: %clang-refold-tester repeated_formal_consistency2 FIRST
// RUN: %clang-refold-tester repeated_formal_consistency2 SECOND
// RUN: %clang-refold-tester repeated_formal_consistency2 EXPAND1
// RUN: %clang-refold-tester repeated_formal_consistency2 EXPAND2
// RUN: %clang-refold-tester repeated_formal_consistency2 EXPAND3
// Test: repeated_formal_consistency2
// Refold intent: preserve when repeated occurrences of the same formal reconstruct consistently

#define DUP(x) ((x) + (x))
#define MIX(x) (DUP(x) * x)
int main(void) {
  int v = (((3) + (3)) * (4 + 1));
  return v == (((3) + (3)) * 2) ? 0 : 1;
}

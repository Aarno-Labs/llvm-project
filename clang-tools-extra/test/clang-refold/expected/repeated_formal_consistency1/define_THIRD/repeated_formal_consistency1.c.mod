// RUN: %clang-refold-tester repeated_formal_consistency1 FIRST
// RUN: %clang-refold-tester repeated_formal_consistency1 SECOND
// RUN: %clang-refold-tester repeated_formal_consistency1 THIRD
// RUN: %clang-refold-tester repeated_formal_consistency1 EXPAND1
// RUN: %clang-refold-tester repeated_formal_consistency1 EXPAND2
// Test: repeated_formal_consistency1
// Refold intent: preserve when repeated occurrences of the same formal reconstruct consistently

#define DUP(x) ((x) + (x))
#define MIX(x) (DUP(x) * 2)
int main(void) {
  int v = MIX(((3) + 4) + 5);
  return v == (((3) + (3)) * 2) ? 0 : 1;
}

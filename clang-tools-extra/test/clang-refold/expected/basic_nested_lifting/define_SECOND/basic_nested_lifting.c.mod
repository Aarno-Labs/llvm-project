// RUN: %clang-refold-tester basic_nested_lifting FIRST
// RUN: %clang-refold-tester basic_nested_lifting SECOND
// RUN: %clang-refold-tester basic_nested_lifting THIRD
// RUN: %clang-refold-tester basic_nested_lifting EXPAND
// Test: basic_nested_lifting
// Refold intent: preserve nested child invocation chain after a local child edit

#define G(x) ((x) + 1)
#define H(x) (G(x) * 3)
int main(void) {
  int v = H((4) + 4);
  return v == (((4) + 1) * 3) ? 0 : 1;
}

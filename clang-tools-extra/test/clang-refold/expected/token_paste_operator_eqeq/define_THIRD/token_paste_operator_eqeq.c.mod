// RUN: %clang-refold-tester token_paste_operator_eqeq FIRST
// RUN: %clang-refold-tester token_paste_operator_eqeq SECOND
// RUN: %clang-refold-tester token_paste_operator_eqeq THIRD
// RUN: %clang-refold-tester token_paste_operator_eqeq EXPAND
// Test: token_paste_operator_eqeq
// Refold intent: cover token paste that forms operators and punctuators instead of identifiers

#define OP2(a, b) a##b
int main(void) {
  int v = (1 OP2(>, =) 1);
  return v ? 0 : 1;
}

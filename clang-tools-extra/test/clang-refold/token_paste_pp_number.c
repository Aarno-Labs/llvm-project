// RUN: %clang-refold-tester token_paste_pp_number FIRST
// RUN: %clang-refold-tester token_paste_pp_number SECOND
// RUN: %clang-refold-tester token_paste_pp_number AMBIGUOUS
// RUN: %clang-refold-tester token_paste_pp_number EXPAND
// Test: token_paste_pp_number
// Refold intent: cover token paste that forms preprocessing numbers rather than identifiers

#define NUMCAT(a, b) a##b
int main(void) {
  int v = NUMCAT(12, 34);
  return v == 1234 ? 0 : 1;
}

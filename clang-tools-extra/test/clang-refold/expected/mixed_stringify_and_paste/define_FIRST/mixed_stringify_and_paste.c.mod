// RUN: %clang-refold-tester mixed_stringify_and_paste FIRST
// RUN: %clang-refold-tester mixed_stringify_and_paste SECOND
// RUN: %clang-refold-tester mixed_stringify_and_paste THIRD
// RUN: %clang-refold-tester mixed_stringify_and_paste EXPAND
// Test: mixed_stringify_and_paste
// Refold intent: handle mixed stringify and paste interactions without losing raw child syntax

#define CAT(x, y) x##y
#define STR(x) #x
#define NAME(prefix, suffix) \
  static const char *s_1 = STR(CAT(prefix, suffix)); \
  int CAT(prefix, suffix)(void) { return 1; }
NAME(bill, z)
int main(void) {
  return billz() == 1 && s_1[0] ? 0 : 1;
}

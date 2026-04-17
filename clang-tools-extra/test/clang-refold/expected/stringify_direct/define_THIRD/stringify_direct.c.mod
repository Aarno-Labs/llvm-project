// RUN: %clang-refold-tester stringify_direct FIRST
// RUN: %clang-refold-tester stringify_direct SECOND
// RUN: %clang-refold-tester stringify_direct THIRD
// RUN: %clang-refold-tester stringify_direct EXPAND
// Test: stringify_direct
// Refold intent: preserve direct stringify of raw formals

#define BAR(x, y) #x "," #y
#define FOO(x, y) BAR(x, y)
static const char *s = FOO(left_alpha, bravo_right);
int main(void) {
  return s[0] ? 0 : 1;
}

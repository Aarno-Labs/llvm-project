// RUN: %clang-refold-tester stringify_wrapper_chain2 FIRST
// RUN: %clang-refold-tester stringify_wrapper_chain2 SECOND
// RUN: %clang-refold-tester stringify_wrapper_chain2 THIRD
// RUN: %clang-refold-tester stringify_wrapper_chain2 EXPAND1
// RUN: %clang-refold-tester stringify_wrapper_chain2 EXPAND2
// Test: stringify_wrapper_chain2
// Refold intent: preserve wrapper-mediated stringify such as STR/XSTR chains

#define STR(x) #x
#define XSTR(x) STR(x)
#define JOIN_INNER(x, y) x##_##y
#define JOIN(x, y) JOIN_INNER(x, y)

static const char *s = XSTR(JOIN(JOIN(JOIN(red, green), blue), yellow));
int main(void) {
  return s[0] ? 0 : 1;
}

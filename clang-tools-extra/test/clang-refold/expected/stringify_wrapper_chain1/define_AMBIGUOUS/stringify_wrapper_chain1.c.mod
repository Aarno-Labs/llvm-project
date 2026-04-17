// RUN: %clang-refold-tester stringify_wrapper_chain1 FIRST
// RUN: %clang-refold-tester stringify_wrapper_chain1 SECOND
// RUN: %clang-refold-tester stringify_wrapper_chain1 THIRD
// RUN: %clang-refold-tester stringify_wrapper_chain1 AMBIGUOUS
// Test: stringify_wrapper_chain1
// Refold intent: preserve wrapper-mediated stringify such as STR/XSTR chains

#define STR(x) #x
#define XSTR(x) STR(x)
#define JOIN_INNER(x, y) x##_##y
#define JOIN(x, y) JOIN_INNER(x, y)

static const char *s = XSTR(JOIN(JOIN(red, green_yellow), blue));
int main(void) {
  return s[0] ? 0 : 1;
}

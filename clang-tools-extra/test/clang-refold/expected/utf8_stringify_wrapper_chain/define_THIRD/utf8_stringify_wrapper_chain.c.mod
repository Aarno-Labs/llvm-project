// RUN: %clang-refold-tester utf8_stringify_wrapper_chain FIRST
// RUN: %clang-refold-tester utf8_stringify_wrapper_chain SECOND
// RUN: %clang-refold-tester utf8_stringify_wrapper_chain THIRD
// RUN: %clang-refold-tester utf8_stringify_wrapper_chain AMBIGUOUS
// Test: utf8_stringify_wrapper_chain
// Refold intent: preserve u8-prefixed wrapper-mediated stringify chains over nested paste trees

#define STR(x) #x
#define XSTR(x) STR(x)
#define U8_INNER(x) u8##x
#define U8(x) U8_INNER(x)
#define U8STR(x) U8(XSTR(x))
#define JOIN_INNER(x, y) x##_##y
#define JOIN(x, y) JOIN_INNER(x, y)

static const char *s = (const char *)U8STR(JOIN(JOIN(black, green), purple));
int main(void) {
  return s[0] ? 0 : 1;
}

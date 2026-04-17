// RUN: %clang-refold-tester wide_stringify_and_widen_nested_join FIRST
// RUN: %clang-refold-tester wide_stringify_and_widen_nested_join SECOND
// RUN: %clang-refold-tester wide_stringify_and_widen_nested_join THIRD
// RUN: %clang-refold-tester wide_stringify_and_widen_nested_join EXPAND
// Test: wide_stringify_and_widen_nested_join
// Refold intent: preserve wide-string wrapper chains over nested paste trees

#define STR(x) #x
#define XSTR(x) STR(x)

#define WIDEN_INNER(x) L##x
#define WIDEN(x) WIDEN_INNER(x)
#define WSTR(x) WIDEN(XSTR(x))

#define JOIN_INNER(x, y) x##_##y
#define JOIN(x, y) JOIN_INNER(x, y)

static const wchar_t *s =
    WSTR(JOIN(JOIN(JOIN(one, zero), three), four));

int main(void) {
  return s[0] ? 0 : 1;
}

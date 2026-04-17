// RUN: %clang-refold-tester variadic_stringify_paste_chain FIRST
// RUN: %clang-refold-tester variadic_stringify_paste_chain SECOND
// RUN: %clang-refold-tester variadic_stringify_paste_chain THIRD
// RUN: %clang-refold-tester variadic_stringify_paste_chain AMBIGUOUS
// Test: variadic_stringify_paste_chain
// Refold intent: cover mixed stringify and paste interactions routed through a variadic formal

#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define STR(x) #x
#define XSTR(x) STR(x)
#define P3V(a, b, ...) XSTR(XCAT(XCAT(a, b), __VA_ARGS__))

static const char *s = P3V(pre_, mid_, sufx);
int main(void) {
  return s[0] ? 0 : 1;
}

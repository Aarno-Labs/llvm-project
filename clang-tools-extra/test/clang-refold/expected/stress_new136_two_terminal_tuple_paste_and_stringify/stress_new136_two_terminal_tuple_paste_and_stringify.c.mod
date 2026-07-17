// RUN: %clang-refold-tester-with-lines stress_new136_two_terminal_tuple_paste_and_stringify
#define DECL(a, b) int a##_value = b;
#define NAME(a, b) const char *name = #a;
#define BOTH(t) DECL t NAME t

BOTH((bar, 2))

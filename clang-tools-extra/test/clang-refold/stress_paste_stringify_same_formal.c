// RUN: %clang-refold-tester-with-lines stress_paste_stringify_same_formal
#define DECL(x) const char *x##_name = #x;

DECL(alpha)

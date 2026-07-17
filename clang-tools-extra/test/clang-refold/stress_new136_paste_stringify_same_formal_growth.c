// RUN: %clang-refold-tester-with-lines stress_new136_paste_stringify_same_formal_growth
#define DECL(x, y) int x##y = 0; const char *name = #x;

DECL(a, bc)

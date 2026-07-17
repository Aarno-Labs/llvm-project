// RUN: %clang-refold-tester-with-lines stress_paste_repeated_formal_growth_first
#define CAT(a, b) a##b
#define USE(x, y) CAT(x, y) + x

int x = USE(a, bc);

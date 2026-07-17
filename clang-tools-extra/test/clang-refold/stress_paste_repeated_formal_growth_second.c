// RUN: %clang-refold-tester-with-lines stress_paste_repeated_formal_growth_second
#define CAT(a, b) a##b
#define USE(x, y) CAT(x, y) + y

int x = USE(ab, c);

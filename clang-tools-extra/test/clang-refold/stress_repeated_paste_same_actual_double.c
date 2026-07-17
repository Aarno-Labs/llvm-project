// RUN: %clang-refold-tester-with-lines stress_repeated_paste_same_actual_double
#define DOUBLE(x) x##x

int v = DOUBLE(foo);

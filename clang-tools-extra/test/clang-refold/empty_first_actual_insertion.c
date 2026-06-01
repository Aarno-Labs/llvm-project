// RUN: %clang-refold-tester-with-lines empty_first_actual_insertion
#define EXPR(a, b) int x = a + b;
EXPR(, 2)

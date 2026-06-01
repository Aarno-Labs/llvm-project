// RUN: %clang-refold-tester-with-lines empty_second_actual_insertion
#define EXPR(a, b) int x = a b;
EXPR(3, + 4)

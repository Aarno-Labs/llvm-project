// RUN: %clang-refold-tester-with-lines empty_middle_actual_insertion
#define EXPR3(a, b, c) int x = a b c;
EXPR3(1, , + 2)

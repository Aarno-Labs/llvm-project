// RUN: %clang-refold-tester-with-lines stress_nested_tuple_two_elements_two_terminals
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))
#define BOTH(f, g, t) f t + g t
#define OUTER(p) BOTH p

int x = OUTER((ADD, SUB, (10, 5)));

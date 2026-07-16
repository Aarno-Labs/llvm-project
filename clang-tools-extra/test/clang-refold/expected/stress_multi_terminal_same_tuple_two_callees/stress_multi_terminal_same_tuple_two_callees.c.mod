// RUN: %clang-refold-tester-with-lines stress_multi_terminal_same_tuple_two_callees
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))
#define BOTH(f, g, t) f t + g t

int x = BOTH(ADD, SUB, (10, 5));

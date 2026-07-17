// RUN: %clang-refold-tester-with-lines stress_recursive_tuple_two_terminal_uses
#define A(f, t) B(f, t, t)
#define B(g, u, v) g u + g v
#define MUL(a, b) ((a) * (b))

int x = A(MUL, (2, 3));

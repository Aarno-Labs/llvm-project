// RUN: %clang-refold-tester recursive_tuple_generated_callee_reject_divergent_repeated_terminal
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) ((h v) + (h v))

#define SUB(a, b) ((a) - (b))

int x = A(SUB, (3, 4));

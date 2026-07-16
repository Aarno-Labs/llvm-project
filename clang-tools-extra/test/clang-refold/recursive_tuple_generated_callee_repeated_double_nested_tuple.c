// RUN: %clang-refold-tester recursive_tuple_generated_callee_repeated_double_nested_tuple
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define ADD(f, t) ((f t) + (f t))
#define WRAP(g, u) g u
#define SUB(a, b) ((a) - (b))

int x = A(ADD, (WRAP, (SUB, (3, 4))));

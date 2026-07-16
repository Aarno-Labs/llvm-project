// RUN: %clang-refold-tester recursive_tuple_generated_callee_nested_repeated_in_wrapper
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define ADD(f, t) ((f t) + (f t))
#define SUB(a, b) ((a) - (b))

int x = A(ADD, (SUB, (15, 20)));

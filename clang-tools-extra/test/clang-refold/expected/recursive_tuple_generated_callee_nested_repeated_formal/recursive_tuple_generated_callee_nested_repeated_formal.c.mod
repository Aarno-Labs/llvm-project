// RUN: %clang-refold-tester recursive_tuple_generated_callee_nested_repeated_formal
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define ADD(f, t) ((f t) + 10)
#define SUB(a, b) (((a) - (a)) + (b))

int x = A(ADD, (SUB, (15, 20)));

// RUN: %clang-refold-tester recursive_tuple_generated_callee_nested
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define SUB1(f, t) f t
#define SUB2(a, b) ((a) - (b))

int x = A(SUB1, (SUB2, (3, 4)));

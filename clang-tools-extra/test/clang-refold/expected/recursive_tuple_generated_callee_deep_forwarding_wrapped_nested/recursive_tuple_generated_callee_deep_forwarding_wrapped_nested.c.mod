// RUN: %clang-refold-tester recursive_tuple_generated_callee_deep_forwarding_wrapped_nested
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) D(h, v)
#define D(i, w) E(i, w)
#define E(j, x) j x

#define ADD(f, t) ((f t) + 10)
#define SUB(a, b) ((a) - (b))

int x = A(ADD, (SUB, (15, 20)));

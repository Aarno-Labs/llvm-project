// RUN: %clang-refold-tester recursive_tuple_generated_callee_forwarding
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v
#define ADD(a,b) ((a)+(b))

int x = A(ADD, (1, 2));

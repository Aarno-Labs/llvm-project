// RUN: %clang-refold-tester recursive_tuple_generated_callee_reordered_forwarding
#define A(f, t) B(f, t)
#define B(g, u) C(u, g)
#define C(v, h) h v

#define ADD(f, t) ((f t) + 10)
#define SUB(a, b) ((a) - (b))

int x = A(ADD, (SUB, (3, 4)));

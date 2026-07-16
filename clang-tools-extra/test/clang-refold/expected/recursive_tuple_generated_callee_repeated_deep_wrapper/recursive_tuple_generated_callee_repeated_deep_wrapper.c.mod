// RUN: %clang-refold-tester recursive_tuple_generated_callee_repeated_deep_wrapper
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define ADD(f, t) ((WRAP1(f, t)) + (WRAP1(f, t)))
#define WRAP1(g, u) WRAP2(g, u)
#define WRAP2(h, v) h v
#define SUB(a, b) ((a) - (b))

int x = A(ADD, (SUB, (15, 20)));

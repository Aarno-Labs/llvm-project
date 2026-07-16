// RUN: %clang-refold-tester recursive_tuple_generated_callee_repeated_forwarded_wrapper
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define ADD(f, t) ((WRAP(f, t)) + (WRAP(f, t)))
#define WRAP(g, u) g u
#define SUB(a, b) ((a) - (b))

int x = A(ADD, (SUB, (15, 20)));

// RUN: %clang-refold-tester recursive_tuple_generated_callee_three_arg_wrapper
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define APPLY(f, t) WRAP(f, t)
#define WRAP(g, u) g u
#define MIX(a, b, c) (((a) * (b)) + (c))

int x = A(APPLY, (MIX, (5, 6, 7)));

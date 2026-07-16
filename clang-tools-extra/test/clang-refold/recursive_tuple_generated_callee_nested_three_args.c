// RUN: %clang-refold-tester recursive_tuple_generated_callee_nested_three_args
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define APPLY(f, t) f t
#define MIX(a, b, c) (((a) * (b)) + (c))

int x = A(APPLY, (MIX, (2, 3, 4)));

// RUN: %clang-refold-tester recursive_tuple_generated_callee_nested_comma_expression
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define APPLY(f, t) ((f t) + 10)
#define PICK(a, b) ((a) + (b))

int x = A(APPLY, (PICK, ((1, 2), 3)));

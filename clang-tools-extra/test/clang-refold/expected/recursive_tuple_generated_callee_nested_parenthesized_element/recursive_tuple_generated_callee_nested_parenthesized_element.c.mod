// RUN: %clang-refold-tester recursive_tuple_generated_callee_nested_parenthesized_element
#define A(f, t) B(f, t)
#define B(g, u) C(g, u)
#define C(h, v) h v

#define ADD(f, t) ((f t) + 10)
#define PICK(a, b) ((a) + (b))

int x = A(ADD, (PICK, ((8 + 9), 40)));

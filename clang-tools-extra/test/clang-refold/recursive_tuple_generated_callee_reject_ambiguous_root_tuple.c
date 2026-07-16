// RUN: %clang-refold-tester recursive_tuple_generated_callee_reject_ambiguous_root_tuple
#define A(f, t1, t2) B(f, t1, t2)
#define B(g, u1, u2) C(g, u1, u2)
#define C(h, v1, v2) h v1

#define SUB(a, b) ((a) - (b))

int x = A(SUB, (3, 4), (3, 4));

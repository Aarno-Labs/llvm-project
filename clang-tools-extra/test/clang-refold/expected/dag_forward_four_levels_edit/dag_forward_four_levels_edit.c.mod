// RUN: %clang-refold-tester dag_forward_four_levels_edit
#define A(x) x
#define B(x) A(x)
#define Cc(x) B(x)
#define D(x) Cc(x)
int v = D(99);

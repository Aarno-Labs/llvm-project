// RUN: %clang-refold-tester dag_vaopt_activation_at_depth
#define LEAF(a,...) a __VA_OPT__(,) __VA_ARGS__
#define MID(a,...) LEAF(a, __VA_ARGS__)
int x[]={1 , 2};

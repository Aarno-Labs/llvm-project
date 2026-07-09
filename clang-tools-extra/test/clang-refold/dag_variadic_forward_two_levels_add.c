// RUN: %clang-refold-tester dag_variadic_forward_two_levels_add
#define INNER(...) f(__VA_ARGS__)
#define OUTER(...) INNER(__VA_ARGS__)
int v = OUTER(1,2);

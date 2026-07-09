// RUN: %clang-refold-tester dag_higher_order_nested_apply
#define APPLY(f,x) f(x)
#define INC(x) ((x)+1)
#define DEC(x) ((x)-1)
int v = APPLY(INC, APPLY(DEC, 88));

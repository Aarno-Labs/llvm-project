// RUN: %clang-refold-tester dag_higher_order_apply_edit_arg
#define APPLY(f,x) f(x)
#define INC(x) ((x)+1)
#define DEC(x) ((x)-1)
int v = APPLY(INC, 77);

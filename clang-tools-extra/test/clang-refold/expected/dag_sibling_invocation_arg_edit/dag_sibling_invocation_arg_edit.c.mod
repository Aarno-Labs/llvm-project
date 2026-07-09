// RUN: %clang-refold-tester dag_sibling_invocation_arg_edit
#define G(x) ((x)+1)
#define H(x) ((x)*2)
int v = G(H(50));

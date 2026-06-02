// RUN: %clang-refold-tester dag_subtree_nested_arg_fixed_body_coedit
#define ID(X) X
#define WRAP(X) f(X) + gap0 + gap1 + gap2 + 100
int v = WRAP(ID(alpha));
int w = WRAP(ID(keep));

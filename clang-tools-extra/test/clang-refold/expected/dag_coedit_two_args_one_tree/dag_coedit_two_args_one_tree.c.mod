// RUN: %clang-refold-tester dag_coedit_two_args_one_tree
#define PAIR(a,b) ((a) + (b))
#define TWICE(a,b) PAIR(a,b) + PAIR(a,b)
int v = TWICE(7, 8);

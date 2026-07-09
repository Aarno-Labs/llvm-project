// RUN: %clang-refold-tester dag_tuple_forward_edit_element
#define UNPACK(f, t) f t
#define ADD(a,b) ((a)+(b))
int v = UNPACK(ADD, (1, 2));

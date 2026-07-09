// RUN: %clang-refold-tester dag_decorated_join_one_slot
#define J(p,a,b) p##_fn(a, b)
#define k_fn(a,b) ((a)*(b))
int v = J(k, 9, 4);

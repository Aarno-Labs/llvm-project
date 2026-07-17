// RUN: %clang-refold-tester-with-lines stress_tuple_empty_middle_actual_edit_edges
#define CALL(f, t) f t
#define EDGES(a, b, c) a + c
#define WRAP(pair) CALL pair

int x = WRAP((EDGES, (1, , 2)));

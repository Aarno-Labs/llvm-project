// RUN: %clang-refold-tester-with-lines stress_tuple_empty_second_actual_edit_first
#define CALL(f, t) f t
#define FIRST(a, b) a
#define WRAP(pair) CALL pair

int x = WRAP((FIRST, (1, )));

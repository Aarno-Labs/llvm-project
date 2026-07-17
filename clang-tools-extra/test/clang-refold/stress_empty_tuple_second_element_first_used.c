// RUN: %clang-refold-tester-with-lines stress_empty_tuple_second_element_first_used
#define CALL(f, t) f t
#define FIRST(a, b) (a)

int x = CALL(FIRST, (5,));

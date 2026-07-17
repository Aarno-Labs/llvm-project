// RUN: %clang-refold-tester-with-lines stress_empty_tuple_first_element_second_used
#define CALL(f, t) f t
#define SECOND(a, b) (b)

int x = CALL(SECOND, (, 5));

// RUN: %clang-refold-tester-with-lines stress_empty_tuple_middle_element_third_used
#define CALL(f, t) f t
#define THIRD(a, b, c) (c)

int x = CALL(THIRD, (1,, 4));

// RUN: %clang-refold-tester-with-lines stress_tuple_forwarded_empty_first_element
#define SECOND(a, b) (b)
#define CALL(f, t) f t
#define WRAP(p) CALL p

int x = WRAP((SECOND, (, 6)));

// RUN: %clang-refold-tester-with-lines stress_tuple_forwarded_to_paste_and_standard_same_actual
#define CATUSE(x) x##Tail + x
#define CALL(f, t) f t
#define WRAP(p) CALL p

int v = WRAP((CATUSE, (bar)));

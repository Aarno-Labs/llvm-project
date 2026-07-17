// RUN: %clang-refold-tester-with-lines stress_tuple_forwarded_to_stringify_and_paste
#define DECLPAIR(x) const char *x##_s = #x;
#define CALL(f, t) f t
#define WRAP(p) CALL p

WRAP((DECLPAIR, (bar)))

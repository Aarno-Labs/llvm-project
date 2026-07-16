// RUN: %clang-refold-tester-with-lines stress_tuple_stringify_nested_parenthesized_actual
#define STR(x) #x
#define CALL(f, t) f t
#define WRAP(p) CALL p

const char *s = WRAP((STR, (alpha + beta)));

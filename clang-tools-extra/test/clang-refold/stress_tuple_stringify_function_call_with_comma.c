// RUN: %clang-refold-tester-with-lines stress_tuple_stringify_function_call_with_comma
#define STR(x) #x
#define CALL(f, t) f t
#define WRAP(p) CALL p

const char *s = WRAP((STR, pair(1, 2)));

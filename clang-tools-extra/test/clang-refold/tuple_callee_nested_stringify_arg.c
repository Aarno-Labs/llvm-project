// RUN: %clang-refold-tester-with-lines tuple_callee_nested_stringify_arg
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR
#define STR(x) #x
#define ID(x) x

const char *s = WRAP((STR, ID(alpha)));

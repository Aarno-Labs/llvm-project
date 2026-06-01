// RUN: %clang-refold-tester-with-lines tuple_callee_stringify_rewrite
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR
#define STR(x) #x

const char *s = WRAP((STR, beta));

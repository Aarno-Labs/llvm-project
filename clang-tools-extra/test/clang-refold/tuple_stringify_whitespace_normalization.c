// RUN: %clang-refold-tester-with-lines tuple_stringify_whitespace_normalization
#define STR(x) #x
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR

const char *s = WRAP((STR, alpha   +   beta));

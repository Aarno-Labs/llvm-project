// RUN: %clang-refold-tester-with-lines tuple_wrapper_suffix_literal
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR + 100
#define ADD_ONE(x) ((x) + 1)

int value = WRAP((ADD_ONE, 10));

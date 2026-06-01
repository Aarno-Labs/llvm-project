// RUN: %clang-refold-tester-with-lines tuple_wrapper_prefix_suffix_literal
#define CALL(F, X) F(X)
#define WRAP(PAIR) 100 + CALL PAIR + 200
#define ADD_ONE(x) ((x) + 1)

int value = WRAP((ADD_ONE, 20));

// RUN: %clang-refold-tester-with-lines tuple_forwarder_object_alias
#define CALL(F, X) F(X)
#define CALL_ALIAS CALL
#define WRAP(PAIR) CALL_ALIAS PAIR
#define ADD_ONE(x) ((x) + 1)

int value = WRAP((ADD_ONE, 10));

// RUN: %clang-refold-tester-with-lines tuple_callee_object_alias_chain
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR
#define FSEL1 FSEL2
#define FSEL2 ADD_ONE
#define ADD_ONE(x) ((x) + 1)

int value = WRAP((FSEL1, 20));

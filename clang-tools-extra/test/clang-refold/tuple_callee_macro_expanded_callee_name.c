// RUN: %clang-refold-tester-with-lines tuple_callee_macro_expanded_callee_name
#define FSEL ADD_ONE
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR
#define ADD_ONE(x) ((x) + 1)

int value = WRAP((FSEL, 10));

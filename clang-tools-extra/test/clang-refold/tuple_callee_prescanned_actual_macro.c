// RUN: %clang-refold-tester-with-lines tuple_callee_prescanned_actual_macro
#define ID(x) x
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR
#define ADD_ONE(x) ((x) + 1)

int value = WRAP((ADD_ONE, ID(10)));

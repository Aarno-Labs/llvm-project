// RUN: %clang-refold-tester-with-lines tuple_callee_duplicate_actual_second
#define CALL2(F, X, Y) F(X, Y)
#define WRAP(PAIR) CALL2 PAIR
#define ADD2(a, b) ((a) + (b))

int value = WRAP((ADD2, 10, 10));

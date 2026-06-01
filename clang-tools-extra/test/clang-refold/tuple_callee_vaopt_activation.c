// RUN: %clang-refold-tester-with-lines tuple_callee_vaopt_activation
#define CALL(F, ...) F(__VA_ARGS__)
#define WRAP(PAIR) CALL PAIR
#define MAYBE_ADD(a, ...) ((a) __VA_OPT__(+ __VA_ARGS__))

int value = WRAP((MAYBE_ADD, 10));

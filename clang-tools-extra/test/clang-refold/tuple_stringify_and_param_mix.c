// RUN: %clang-refold-tester-with-lines tuple_stringify_and_param_mix
#define FIELD(name, value) #name ":" value
#define CALL(F, A, B) F(A, B)
#define WRAP(PAIR) CALL PAIR

const char *s = WRAP((FIELD, alpha, "10"));

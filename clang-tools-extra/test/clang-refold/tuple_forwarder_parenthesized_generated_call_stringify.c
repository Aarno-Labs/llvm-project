// RUN: %clang-refold-tester-with-lines tuple_forwarder_parenthesized_generated_call_stringify
#define STR(x) #x
#define CALL(F, X) (F(X))
#define WRAP(PAIR) CALL PAIR

const char *s = WRAP((STR, alpha));

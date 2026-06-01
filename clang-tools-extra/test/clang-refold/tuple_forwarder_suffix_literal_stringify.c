// RUN: %clang-refold-tester-with-lines tuple_forwarder_suffix_literal_stringify
#define STR(x) #x
#define CALL(F, X) F(X) "!"
#define WRAP(PAIR) CALL PAIR

const char *s = WRAP((STR, alpha));

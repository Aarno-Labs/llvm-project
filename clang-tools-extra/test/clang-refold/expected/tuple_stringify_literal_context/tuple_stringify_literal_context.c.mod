// RUN: %clang-refold-tester-with-lines tuple_stringify_literal_context
#define LABEL(x) "[" #x "]"
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR

const char *s = WRAP((LABEL, beta));

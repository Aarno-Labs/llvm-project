// RUN: %clang-refold-tester nested_macros5
#define STR(X) #X
#define OUTER(X,Y) STR(X) STR(Y)

const char *s = OUTER(foo, bar); // "foo""bar" -> "foobar"

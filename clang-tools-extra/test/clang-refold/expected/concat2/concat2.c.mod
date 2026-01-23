// RUN: %clang-refold-tester concat2
#define BAR(X, Y) printf(X, Y)
#define FOO(X) BAR("hello: %d\n", X)
#define HELLO(X) FOO(X##.0)
HELLO(11);

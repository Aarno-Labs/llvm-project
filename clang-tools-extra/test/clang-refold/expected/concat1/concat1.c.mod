// RUN: %clang-refold-tester concat1
#define FOO(X,Y) int X##_##Y = 0
int joe_bob = 0;

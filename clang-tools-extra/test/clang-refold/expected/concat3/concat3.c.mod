// RUN: %clang-refold-tester concat3
#define FOO(X,Y) int X##_##Y = 0
FOO(joe, bob);

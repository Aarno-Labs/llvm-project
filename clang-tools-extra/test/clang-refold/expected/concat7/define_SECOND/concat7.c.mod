// RUN: %clang-refold-tester concat7 FIRST
// RUN: %clang-refold-tester concat7 SECOND
// RUN: %clang-refold-tester concat7 THIRD
// RUN: %clang-refold-tester concat7 MULTI
// RUN: %clang-refold-tester concat7 ALL
// RUN: %clang-refold-tester concat7 DELETE
#define CONCAT(X, Y, Z) X##_##Y##_##Z
int a_foo_c  = 5;
int x = CONCAT(a, foo, c);

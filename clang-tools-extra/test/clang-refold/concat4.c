// RUN: %clang-refold-tester concat4
// RUN: %clang-refold-tester concat4 OTHER
#define PASTE(x, y) x##y
PASTE(a + b, c)
PASTE(a, b + c)

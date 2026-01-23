// RUN: %clang-refold-tester concat4
// RUN: %clang-refold-tester concat4 OTHER
#define PASTE(x, y) x##y
PASTE(a + z, c)
PASTE(a, b + c)

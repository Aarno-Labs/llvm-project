// RUN: %clang-refold-tester concat6 FIRST
// RUN: %clang-refold-tester concat6 SECOND
#define PASTE(x, y) x##y
PASTE((a + b)z, c)
PASTE(a, b + z)

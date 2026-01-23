// RUN: %clang-refold-tester concat6 FIRST
// RUN: %clang-refold-tester concat6 SECOND
#define PASTE(x, y) x##y
PASTE((a + h)z, c)
PASTE(z, b + h)

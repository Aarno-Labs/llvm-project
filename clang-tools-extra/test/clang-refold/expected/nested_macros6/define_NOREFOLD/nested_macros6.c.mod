// RUN: %clang-refold-tester nested_macros6 REFOLD
// RUN: %clang-refold-tester nested_macros6 NOREFOLD
#define CAT(a,b) a##_##b
#define XCAT(a,b) CAT(a,b)

#define P3(a,b,c) XCAT(a, XCAT(b,c))

joe_bob_briggs_z   // expands to abc

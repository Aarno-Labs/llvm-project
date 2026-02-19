// RUN: %clang-refold-tester nested_macros6 REFOLD
// RUN: %clang-refold-tester nested_macros6 NOREFOLD
#define CAT(a,b) a##_##b
#define XCAT(a,b) CAT(a,b)

#define P3(a,b,c) XCAT(a, XCAT(b,c))

P3(x,y,z)   // expands to abc

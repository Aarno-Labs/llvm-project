// RUN: %clang-refold-tester nested_macros3 MULTIPARENT
// RUN: %clang-refold-tester nested_macros3 ROOTOWNED
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define P3(a,b,c) XCAT(a, XCAT(b,c))

adc   // expands to abc

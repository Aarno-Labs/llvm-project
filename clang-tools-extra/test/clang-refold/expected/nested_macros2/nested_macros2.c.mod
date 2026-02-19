// RUN: %clang-refold-tester nested_macros2
#define P(X) X##u
#define S(X) #X
S(P(11))     // "10u"

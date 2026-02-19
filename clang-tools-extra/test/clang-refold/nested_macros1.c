// RUN: %clang-refold-tester nested_macros1
#define STRINGIFY(X) #X
#define PASTE(X,Y) STRINGIFY(X##Y)
PASTE(joe, bob)

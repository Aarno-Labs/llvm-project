// RUN: %clang-refold-tester concat5
#define PASTE(X,Y) X##Y
PASTE(joe, bob)

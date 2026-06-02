// RUN: %clang-refold-tester stringify_and_fixed_body_coedit
#define WRAP(X) f(#X, 100)
int v = WRAP(foo);
int w = WRAP(keep);

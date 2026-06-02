// RUN: %clang-refold-tester stringify_and_fixed_body_coedit
#define WRAP(X) f(#X, 100)
int v = f("bar", 200);
int w = WRAP(keep);

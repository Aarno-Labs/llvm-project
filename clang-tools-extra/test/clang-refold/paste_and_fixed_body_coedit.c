// RUN: %clang-refold-tester paste_and_fixed_body_coedit
#define WRAP(X) f(pre_##X, 100)
int v = WRAP(foo);
int w = WRAP(keep);

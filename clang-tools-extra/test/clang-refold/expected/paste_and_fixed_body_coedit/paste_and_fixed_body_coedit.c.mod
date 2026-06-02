// RUN: %clang-refold-tester paste_and_fixed_body_coedit
#define WRAP(X) f(pre_##X, 100)
int v = f(pre_bar, 200);
int w = WRAP(keep);

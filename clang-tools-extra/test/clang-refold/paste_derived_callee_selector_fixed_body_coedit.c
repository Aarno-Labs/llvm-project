// RUN: %clang-refold-tester paste_derived_callee_selector_fixed_body_coedit
#define CALLEE_0(X) ((X) + 1)
#define CALLEE_1(X) ((X) + 2)
#define WRAP(N, X) CALLEE_##N(X) + gap0 + gap1 + gap2 + 100
int v = WRAP(0, alpha);
int w = WRAP(0, keep);

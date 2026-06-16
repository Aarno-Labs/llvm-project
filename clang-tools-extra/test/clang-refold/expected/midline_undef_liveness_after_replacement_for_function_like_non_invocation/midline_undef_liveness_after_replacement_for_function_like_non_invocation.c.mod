// RUN: %clang-refold-tester-with-lines midline_undef_liveness_after_replacement_for_function_like_non_invocation
#define M() 10
int prefix = 0; long
M;
#undef M

int M(void);
int use = M();

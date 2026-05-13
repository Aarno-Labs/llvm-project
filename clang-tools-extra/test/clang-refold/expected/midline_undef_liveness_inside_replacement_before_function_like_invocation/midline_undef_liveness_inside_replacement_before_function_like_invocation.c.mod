// RUN: %clang-refold-tester-with-lines midline_undef_liveness_inside_replacement_before_function_like_invocation
#define M() 10
int prefix = 0; long
#undef M
M();
#line 6 "midline_undef_liveness_inside_replacement_before_function_like_invocation.c"

int M(void);
int use = M();

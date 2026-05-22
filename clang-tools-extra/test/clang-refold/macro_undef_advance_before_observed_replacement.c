// RUN: %clang-refold-tester-with-lines macro_undef_advance_before_observed_replacement
#define M 10
int before = 0;
#undef M
int bridge = 1;
int keep = M;

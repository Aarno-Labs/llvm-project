// RUN: %clang-refold-tester-with-lines macro_undef_advance_before_observed_replacement
#define M 10
#undef M
int before = M + 2;
int keep = M;

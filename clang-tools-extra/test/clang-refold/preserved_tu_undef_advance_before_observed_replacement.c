// RUN: %clang-refold-tester-with-lines preserved_tu_undef_advance_before_observed_replacement
#define M 10
int before = 0;
#undef M
int keep = M;

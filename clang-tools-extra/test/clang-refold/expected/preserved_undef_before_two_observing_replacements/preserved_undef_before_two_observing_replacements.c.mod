// RUN: %clang-refold-tester-with-lines preserved_undef_before_two_observing_replacements
#define M 10
#undef M
int first = M + 1;
int second = M + 2;
int keep = M;

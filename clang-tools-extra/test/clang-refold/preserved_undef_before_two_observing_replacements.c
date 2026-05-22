// RUN: %clang-refold-tester-with-lines preserved_undef_before_two_observing_replacements
#define M 10
int first = 0;
#undef M
int second = 0;
int keep = M;

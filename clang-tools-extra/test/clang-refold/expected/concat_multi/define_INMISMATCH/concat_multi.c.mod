// RUN: %clang-refold-tester concat_multi INSIDE
// RUN: %clang-refold-tester concat_multi OUTSIDE
// RUN: %clang-refold-tester concat_multi OUTSINGLE
// RUN: %clang-refold-tester concat_multi INSINGLE
// RUN: %clang-refold-tester concat_multi BOTH
// RUN: %clang-refold-tester concat_multi INMISMATCH
// RUN: %clang-refold-tester concat_multi OUTMISMATCH
#define CONCAT(X, Y, Z) X##_##Y##_##Z##_##Y##_##X
int a_bar_c_baz_a  = 5;
int x = a_bar_c_baz_a;

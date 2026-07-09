// RUN: %clang-refold-tester pragma_directive_two_adjacent_diagnostics
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wx"
int a = 1;

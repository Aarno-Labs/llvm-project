// RUN: %clang-refold-tester-with-lines mixed_tu_include_with_unrelated_tu_macro_call_gap
#define ID(x) x
int prefix = 0;
int a = 10;
int gap = ID(7);
int b = 20;
int suffix = 0;

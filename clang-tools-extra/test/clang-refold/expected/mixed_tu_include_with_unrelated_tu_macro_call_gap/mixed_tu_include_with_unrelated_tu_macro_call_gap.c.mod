// RUN: %clang-refold-tester-with-lines mixed_tu_include_with_unrelated_tu_macro_call_gap
#define ID(x) x
int prefix = 0;
#line 1 "headers/value_a.h"
int a = 10;
#line 5 "mixed_tu_include_with_unrelated_tu_macro_call_gap.c"
int gap = ID(7);
#line 1 "headers/value_b.h"
int b = 20;
#line 7 "mixed_tu_include_with_unrelated_tu_macro_call_gap.c"
int suffix = 0;

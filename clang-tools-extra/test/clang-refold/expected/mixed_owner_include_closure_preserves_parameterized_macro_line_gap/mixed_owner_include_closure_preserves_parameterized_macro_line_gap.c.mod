// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_parameterized_macro_line_gap
#define LINE_NO(x) x
int x =
3
#line 124 "gap.c"
;
int y = __LINE__;

// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_variadic_line_macro_gap
#define LINE_NO(...) 123
int x =
3
#line 124 "gap.c"
;
int y = __LINE__;

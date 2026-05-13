// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_nested_builtin_line_macro_gap
#define LINE_NO(x) x
int x =
3
#line 6 "gap.c"
;
int y = __LINE__;

// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_macro_line_gap
#define GAP_LINE 123
int x =
3
#line GAP_LINE "gap.c"

;
int y = __LINE__;

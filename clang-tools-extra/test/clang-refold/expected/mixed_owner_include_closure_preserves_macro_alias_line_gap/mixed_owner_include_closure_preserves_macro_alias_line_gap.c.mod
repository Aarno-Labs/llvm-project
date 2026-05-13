// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_macro_alias_line_gap
#define GAP_ALIAS GAP_LINE
#define GAP_LINE 123
int x =
3
#line 124 "gap.c"
;
int y = __LINE__;

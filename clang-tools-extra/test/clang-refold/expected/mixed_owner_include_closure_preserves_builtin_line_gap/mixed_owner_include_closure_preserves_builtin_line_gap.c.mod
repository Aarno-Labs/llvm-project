// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_builtin_line_gap
int x =
3
#line 5 "gap.c"
;
int y = __LINE__;

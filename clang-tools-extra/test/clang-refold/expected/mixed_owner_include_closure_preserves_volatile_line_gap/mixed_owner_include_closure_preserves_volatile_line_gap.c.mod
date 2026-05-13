// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_volatile_line_gap
int x =
3
#line 124 "mixed_owner_include_closure_preserves_volatile_line_gap.c"
;
int y = __LINE__;

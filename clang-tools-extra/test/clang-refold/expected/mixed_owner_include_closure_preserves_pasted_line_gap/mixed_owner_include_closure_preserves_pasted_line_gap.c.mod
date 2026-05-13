// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_pasted_line_gap
#define NUM(a,b) a ## b
int x =
3
#line 124 "gap.c"
;
int y = __LINE__;

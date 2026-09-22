// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_delete_preserves_line_gap
int x =
#line 123 "gap.c"

;
int y = __LINE__;

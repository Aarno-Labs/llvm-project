// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_spliced_line_gap
int x =
3
#\
line 123 "gap.c"

;
int y = __LINE__;

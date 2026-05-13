// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_spliced_line_gap
int x =
1 +
#\
line 123 "gap.c"
#include "two.inc"
;
int y = __LINE__;

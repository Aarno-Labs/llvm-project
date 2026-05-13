// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_spliced_builtin_line_gap
int x =
1 +
#\
line __LINE__ "gap.c"
#include "two.inc"
;
int y = __LINE__;

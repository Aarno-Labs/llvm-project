// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_pasted_line_gap
#define NUM(a,b) a ## b
int x =
1 +
#line NUM(12,3) "gap.c"
#include "two.inc"
;
int y = __LINE__;

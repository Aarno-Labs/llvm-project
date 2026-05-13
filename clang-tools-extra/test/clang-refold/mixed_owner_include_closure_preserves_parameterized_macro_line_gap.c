// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_parameterized_macro_line_gap
#define LINE_NO(x) x
int x =
1 +
#line LINE_NO(123) "gap.c"
#include "two.inc"
;
int y = __LINE__;

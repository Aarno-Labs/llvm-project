// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_macro_alias_line_gap
#define GAP_ALIAS GAP_LINE
#define GAP_LINE 123
int x =
1 +
#line GAP_ALIAS "gap.c"
#include "two.inc"
;
int y = __LINE__;

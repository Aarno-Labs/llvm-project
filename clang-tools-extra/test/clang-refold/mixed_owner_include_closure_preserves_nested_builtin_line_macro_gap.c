// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_nested_builtin_line_macro_gap
#define LINE_NO(x) x
int x =
1 +
#line LINE_NO(__LINE__) "gap.c"
#include "two.inc"
;
int y = __LINE__;

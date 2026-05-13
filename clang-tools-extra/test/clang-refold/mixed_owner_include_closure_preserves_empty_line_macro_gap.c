// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_empty_line_macro_gap
#define EMPTY
int x =
1 +
#line EMPTY 123 "gap.c"
#include "two.inc"
;
int y = __LINE__;
const char *f = __FILE__;

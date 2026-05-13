// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_volatile_line_gap
int x =
1 +
#line 123 __DATE__
#include "two.inc"
;
int y = __LINE__;

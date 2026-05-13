// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_linemarker_gap
int x =
1 +
# 123 "gap.c"
#include "two.inc"
;
int y = __LINE__;

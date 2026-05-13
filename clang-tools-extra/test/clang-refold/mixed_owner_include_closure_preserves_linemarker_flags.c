// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_linemarker_flags
int x =
1 +
# 123 "gap.c" 1
#include "two.inc"
;
int y = __LINE__;

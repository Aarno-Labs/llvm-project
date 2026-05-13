// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_linemarker_gap
int x =
3
#line 124 "gap.c"
;
int y = __LINE__;

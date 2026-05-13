// RUN: %clang-refold-tester-with-lines mixed_include_macro_condition_selected_zero_token_include_gap
#define ENABLE 1
int x =
3
#if ENABLE
#include "empty.inc"
#endif
;
#line 10 "mixed_include_macro_condition_selected_zero_token_include_gap.c"
int y = __LINE__;

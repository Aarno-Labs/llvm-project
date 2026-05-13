// RUN: %clang-refold-tester-with-lines mixed_include_macro_condition_selected_macro_state_include_gap
#define ENABLE 1
int x =
3
#if ENABLE
#include "pragma_once_define_gap.inc"
#endif
;
#line 10 "mixed_include_macro_condition_selected_macro_state_include_gap.c"
int y = GAP_VALUE;

// RUN: %clang-refold-tester-with-lines header_macro_condition_selected_macro_state_include_gap
#line 1 "headers/outer_state_gap.h"
#define ENABLE 1
int x =
3
#if ENABLE
#include "define_gap.inc"
#endif
#line 8 "headers/outer_state_gap.h"
;
#line 3 "header_macro_condition_selected_macro_state_include_gap.c"
int y = GAP_VALUE;

// RUN: %clang-refold-tester-with-lines header_macro_condition_selected_macro_state_include_gap
#define ENABLE 1
int x =
3
#if ENABLE
#include "define_gap.inc"
#endif
;
int y = GAP_VALUE;

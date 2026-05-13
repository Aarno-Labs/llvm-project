// RUN: %clang-refold-tester-with-lines mixed_include_macro_condition_selected_macro_state_gap
#define ENABLE 1
int x =
3
#if ENABLE
#define GAP_VALUE 99
#endif
;
#line 10 "mixed_include_macro_condition_selected_macro_state_gap.c"
int y = GAP_VALUE + __LINE__;

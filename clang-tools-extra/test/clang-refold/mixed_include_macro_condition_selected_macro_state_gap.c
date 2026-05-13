// RUN: %clang-refold-tester-with-lines mixed_include_macro_condition_selected_macro_state_gap
#define ENABLE 1
int x =
1 +
#if ENABLE
#define GAP_VALUE 99
#endif
#include "two.inc"
;
int y = GAP_VALUE + __LINE__;

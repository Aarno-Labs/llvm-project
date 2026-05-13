// RUN: %clang-refold-tester-with-lines header_macro_condition_selected_macro_state_include_gap
#include "outer_state_gap.h"
int y = GAP_VALUE;

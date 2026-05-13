// RUN: %clang-refold-tester-with-lines mixed_include_macro_condition_selected_zero_token_macro_gap
#define ENABLE 1
#define EMPTY
int x =
1 +
#if ENABLE
EMPTY
#endif
#include "two.inc"
;
int y = __LINE__;

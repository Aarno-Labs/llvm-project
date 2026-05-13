// RUN: %clang-refold-tester-with-lines mixed_include_macro_condition_selected_zero_token_include_gap
#define ENABLE 1
int x =
1 +
#if ENABLE
#include "empty.inc"
#endif
#include "two.inc"
;
int y = __LINE__;

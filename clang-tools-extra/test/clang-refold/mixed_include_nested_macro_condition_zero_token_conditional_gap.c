// RUN: %clang-refold-tester-with-lines mixed_include_nested_macro_condition_zero_token_conditional_gap
#define ENABLE 1
#define ZERO 0
int x =
1 +
#if ENABLE
#if ZERO
int hidden = should_not_appear;
#endif
#endif
#include "two.inc"
;
int y = __LINE__;

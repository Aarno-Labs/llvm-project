// RUN: %clang-refold-tester-with-lines mixed_include_macro_condition_zero_token_conditional_gap
#define ZERO 0
int x =
1 +
#if ZERO
int hidden = should_not_appear;
#endif
#include "two.inc"
;
int y = __LINE__;

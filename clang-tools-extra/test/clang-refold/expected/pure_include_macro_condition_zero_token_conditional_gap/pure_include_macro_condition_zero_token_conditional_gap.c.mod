// RUN: %clang-refold-tester-with-lines pure_include_macro_condition_zero_token_conditional_gap
#define ZERO 0
int x =
3
#if ZERO
int hidden = should_not_appear;
#endif
;
#line 10 "pure_include_macro_condition_zero_token_conditional_gap.c"
int y = __LINE__;

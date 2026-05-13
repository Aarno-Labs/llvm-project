// RUN: %clang-refold-tester-with-lines mixed_include_macro_condition_selected_zero_token_macro_gap
#define ENABLE 1
#define EMPTY
int x =
3
#if ENABLE
EMPTY
#endif
;
#line 11 "mixed_include_macro_condition_selected_zero_token_macro_gap.c"
int y = __LINE__;

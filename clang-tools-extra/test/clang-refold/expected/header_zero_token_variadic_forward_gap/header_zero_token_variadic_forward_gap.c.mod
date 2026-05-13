// RUN: %clang-refold-tester-with-lines header_zero_token_variadic_forward_gap
#line 1 "headers/forward_gap.h"
#define EMPTY
#define FORWARD(...) __VA_ARGS__
int x =
3
FORWARD(EMPTY)
;
#line 3 "header_zero_token_variadic_forward_gap.c"
int y = __LINE__;

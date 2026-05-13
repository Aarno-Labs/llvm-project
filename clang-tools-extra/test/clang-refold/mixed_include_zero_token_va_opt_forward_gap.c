// RUN: %clang-refold-tester-with-lines mixed_include_zero_token_va_opt_forward_gap
#define EMPTY
#define FORWARD(...) __VA_OPT__(__VA_ARGS__)
int x =
1 + FORWARD(EMPTY)
#include "two.inc"
;
int y = __LINE__;

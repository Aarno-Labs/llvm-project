// RUN: %clang-refold-tester-with-lines mixed_include_zero_token_variadic_forward_gap
#define EMPTY
#define FORWARD(...) __VA_ARGS__
int x =
3
FORWARD(EMPTY)
;
int y = __LINE__;

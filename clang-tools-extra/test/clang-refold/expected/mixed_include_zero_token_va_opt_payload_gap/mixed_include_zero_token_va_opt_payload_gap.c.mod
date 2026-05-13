// RUN: %clang-refold-tester-with-lines mixed_include_zero_token_va_opt_payload_gap
#define EMPTY
#define FORWARD(...) __VA_OPT__(EMPTY)
int x =
3
FORWARD(1)
;
int y = 7;

// RUN: %clang-refold-tester mixed_include_zero_token_paste_placemarker_gap
#define CAT(a,b) a ## b
int x =
3
CAT(,)
;
int y = 7;

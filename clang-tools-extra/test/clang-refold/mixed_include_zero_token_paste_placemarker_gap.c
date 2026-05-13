// RUN: %clang-refold-tester mixed_include_zero_token_paste_placemarker_gap
#define CAT(a,b) a ## b
int x =
1 + CAT(,)
#include "two.inc"
;
int y = 7;

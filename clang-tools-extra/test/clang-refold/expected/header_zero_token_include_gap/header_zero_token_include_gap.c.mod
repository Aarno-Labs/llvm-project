// RUN: %clang-refold-tester header_zero_token_include_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int before = 10,
#include "empty.inc"
 after = 20;

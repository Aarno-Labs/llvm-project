// RUN: %clang-refold-tester header_zero_token_include_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#include "include_gap_outer.h"

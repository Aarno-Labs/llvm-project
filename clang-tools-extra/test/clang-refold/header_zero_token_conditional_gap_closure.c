// RUN: %clang-refold-tester header_zero_token_conditional_gap_closure
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#include "cond_gap.h"

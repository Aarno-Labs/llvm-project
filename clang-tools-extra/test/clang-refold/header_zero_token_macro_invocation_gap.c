// RUN: %clang-refold-tester header_zero_token_macro_invocation_gap
#define KEEP(x) ((x) + 1)
#define EMPTY()
#define WRAP_EMPTY() EMPTY()

int untouched = KEEP(5);

#include "macro_gap.h"

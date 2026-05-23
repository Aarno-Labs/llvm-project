// RUN: %clang-refold-tester-with-lines repeated_zero_token_header_duplicate_pragmas_bind_order
#include "h_dupe_pragma_only.h"
#pragma vendor note
int value = 2;

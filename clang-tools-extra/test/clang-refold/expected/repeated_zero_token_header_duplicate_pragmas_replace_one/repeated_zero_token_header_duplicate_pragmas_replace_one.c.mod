// RUN: %clang-refold-tester-with-lines repeated_zero_token_header_duplicate_pragmas_replace_one
#include "h_dupe_pragma_replace.h"
#pragma vendor beta
#pragma vendor note
int value = 2;

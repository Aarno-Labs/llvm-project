// RUN: %clang-refold-tester-with-lines repeated_zero_token_header_duplicate_pragmas_replace_one
#include "h_dupe_pragma_replace.h"
#line 1 "headers/h_dupe_pragma_replace.h"
#pragma vendor beta
#pragma vendor note
#line 4 "repeated_zero_token_header_duplicate_pragmas_replace_one.c"
int value = 2;

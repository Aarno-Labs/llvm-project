// RUN: %clang-refold-tester-with-lines adjacent_zero_token_repeated_header_replace_second
#include "pragma_only.h"
#line 1 "headers/pragma_only.h"
#pragma vendor changed
#line 4 "adjacent_zero_token_repeated_header_replace_second.c"
int value = 2;

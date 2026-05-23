// RUN: %clang-refold-tester-with-lines adjacent_zero_token_repeated_header_replace_second
#include "pragma_only.h"
#pragma vendor changed
int value = 2;

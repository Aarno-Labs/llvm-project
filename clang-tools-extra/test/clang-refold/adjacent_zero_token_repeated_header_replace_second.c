// RUN: %clang-refold-tester-with-lines adjacent_zero_token_repeated_header_replace_second
#include "pragma_only.h"
#include "pragma_only.h"
int value = 1;

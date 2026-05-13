// RUN: %clang-refold-tester header_zero_token_function_like_macro_arg_gap
#define EMPTY()
#define PASS(x) x

#include "pass_gap.h"

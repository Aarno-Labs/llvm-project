// RUN: %clang-refold-tester-with-lines empty_macro_state_include_between_materialized_header_and_suffix
#include "define_k.h"
#line 1 "headers/use_k.h"
int use = K+ 1;
#line 4 "empty_macro_state_include_between_materialized_header_and_suffix.c"
#include "undef_k.h"
int after = K;

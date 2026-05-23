// RUN: %clang-refold-tester-with-lines empty_macro_state_include_between_materialized_header_and_suffix
#include "define_k.h"
int use = K+ 1;
#include "undef_k.h"
int after = K;

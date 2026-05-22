// RUN: %clang-refold-tester-with-lines empty_macro_state_include_between_materialized_header_and_suffix
#include "define_k.h"
#include "use_k.h"
#include "undef_k.h"
int after = K;

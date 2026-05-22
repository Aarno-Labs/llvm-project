// RUN: %clang-refold-tester-with-lines include_owned_define_delay_before_observed_replacement
int before = M + 2;
#include "define_m.h"
#include "use_m.h"

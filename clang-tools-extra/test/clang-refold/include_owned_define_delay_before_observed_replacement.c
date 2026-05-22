// RUN: %clang-refold-tester-with-lines include_owned_define_delay_before_observed_replacement
#include "define_m.h"
int before = 0;
#include "use_m.h"

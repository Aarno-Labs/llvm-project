// RUN: %clang-refold-tester-with-lines include_owned_undef_advance_before_observed_replacement
#define M 10
int before = 0;
#include "undef_m.h"
int keep = M;

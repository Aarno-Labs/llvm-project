// RUN: %clang-refold-tester-with-lines include_owned_undef_advance_before_observed_replacement
#define M 10
#include "undef_m.h"
int before = M + 2;
int keep = M;

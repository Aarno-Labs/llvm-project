// RUN: %clang-refold-tester-with-lines macro_define_bol_observed_replacement_include_liveness
int dead_a = 1;
#define PAYLOAD 100
int dead_b = 2;
#include "uses_payload.h"

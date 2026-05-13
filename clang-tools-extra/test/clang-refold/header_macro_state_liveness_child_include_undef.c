// RUN: %clang-refold-tester header_macro_state_liveness_child_include_undef
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#include "state_outer.h"

// RUN: %clang-refold-tester header_macro_state_liveness_same_decl_undef
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#include "state_same_decl_undef.h"

// RUN: %clang-refold-tester header_macro_state_liveness_material_child_include
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#include "state_outer2.h"

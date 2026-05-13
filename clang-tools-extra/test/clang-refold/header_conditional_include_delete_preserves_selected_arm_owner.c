// RUN: %clang-refold-tester header_conditional_include_delete_preserves_selected_arm_owner
#define KEEP(x) ((x) + 1)
#define ENABLE_GAP 1

int untouched = KEEP(5);

#include "outer.h"

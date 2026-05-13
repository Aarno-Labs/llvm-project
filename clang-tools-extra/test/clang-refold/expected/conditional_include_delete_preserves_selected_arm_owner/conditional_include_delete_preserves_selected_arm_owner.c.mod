// RUN: %clang-refold-tester conditional_include_delete_preserves_selected_arm_owner
#define KEEP(x) ((x) + 1)
#define ENABLE_GAP 1

int untouched = KEEP(5);

int x[] = {
#include "x.h"
#ifdef ENABLE_GAP
4,
#include "z.h"

#endif
};

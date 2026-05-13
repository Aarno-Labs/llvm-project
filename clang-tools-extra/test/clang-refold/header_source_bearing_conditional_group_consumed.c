// RUN: %clang-refold-tester header_source_bearing_conditional_group_consumed
#define KEEP(x) ((x) + 1)
#define ENABLE_MIDDLE 1

int untouched = KEEP(5);

#include "cond_material.h"

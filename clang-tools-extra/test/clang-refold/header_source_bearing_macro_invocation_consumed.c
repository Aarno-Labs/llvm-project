// RUN: %clang-refold-tester header_source_bearing_macro_invocation_consumed
#define KEEP(x) ((x) + 1)
#define EMIT_MIDDLE() int middle = 99;

int untouched = KEEP(5);

#include "macro_material.h"

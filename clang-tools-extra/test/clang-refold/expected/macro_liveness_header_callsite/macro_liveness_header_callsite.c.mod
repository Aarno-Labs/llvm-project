// RUN: %clang-refold-tester macro_liveness_header_callsite
#define KEEP(x) ((x) + 1)
int untouched = KEEP(5);

int before = 10,
#define VALUE 7
 middle = 20;
#include "use_value.h"
int after = 3;

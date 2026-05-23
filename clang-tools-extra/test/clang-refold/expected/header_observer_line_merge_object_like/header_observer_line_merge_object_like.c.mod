// RUN: %clang-refold-tester-with-lines header_observer_line_merge_object_like
#include "loc_defs.h"
#line LOC
int a = 1; int observed = 501;

// RUN: %clang-refold-tester-with-lines imported_line_merge
#include "loc_defs.h"
#line LOC
int a = 1;
int observed = __LINE__;

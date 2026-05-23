// RUN: %clang-refold-tester-with-lines imported_line_loc
#include "loc_defs.h"
#line LOC
int observed = __LINE__;

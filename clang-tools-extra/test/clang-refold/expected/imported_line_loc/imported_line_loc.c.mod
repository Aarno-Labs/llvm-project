// RUN: %clang-refold-tester-with-lines imported_line_loc
#include "loc_defs.h"
#line LOC
int inserted = 0;
#line 500 "logical_imported.c"
int observed = __LINE__;

// RUN: %clang-refold-tester-with-lines imported_func_line_loc
#include "loc_func_defs.h"
#line LOC(700, "logical_func_imported.c")
int inserted = 0;
#line 700 "logical_func_imported.c"
int observed = __LINE__;

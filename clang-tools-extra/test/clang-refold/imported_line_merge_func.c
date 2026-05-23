// RUN: %clang-refold-tester-with-lines imported_line_merge_func
#include "loc_func_defs.h"
#line LOC(700, "logical_func_imported.c")
int a = 1;
int observed = __LINE__;

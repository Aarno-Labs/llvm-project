// RUN: %clang-refold-tester-with-lines pending_resync_function_like
#include "loc_func_defs.h"
#line LOC(700, "logical_func_imported.c")
int a = 1; int b = 2;
#line 702 "logical_func_imported.c"
int observed = __LINE__;

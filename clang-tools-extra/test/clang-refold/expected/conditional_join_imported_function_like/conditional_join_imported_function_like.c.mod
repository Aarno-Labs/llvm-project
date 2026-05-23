// RUN: %clang-refold-tester-with-lines conditional_join_imported_function_like
#include "loc_func_defs.h"
#if 1
#line LOC(1200, "logical_cond_func.c")
int inserted = 0;
int inside = 1;
#endif
#line 1202 "logical_cond_func.c"
int observed = __LINE__;

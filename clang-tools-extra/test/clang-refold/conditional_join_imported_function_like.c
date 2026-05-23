// RUN: %clang-refold-tester-with-lines conditional_join_imported_function_like
#include "loc_func_defs.h"
#if 1
#line LOC(1200, "logical_cond_func.c")
int inside = 1;
#endif
int observed = __LINE__;

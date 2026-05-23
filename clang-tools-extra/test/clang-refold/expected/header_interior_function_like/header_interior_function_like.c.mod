// RUN: %clang-refold-tester-with-lines header_interior_function_like
#include "loc_func_defs.h"
#if 1
#line LOC(1200, "logical_header_inner_func.c")
int first = 0;
int inserted = 0;
#line 1201 "logical_header_inner_func.c"
int inside = __LINE__;
#endif
#line 1203 "logical_header_inner_func.c"
int observed = __LINE__;

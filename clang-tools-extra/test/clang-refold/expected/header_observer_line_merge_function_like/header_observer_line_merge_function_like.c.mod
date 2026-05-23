// RUN: %clang-refold-tester-with-lines header_observer_line_merge_function_like
#include "loc_func_defs.h"
#line LOC(700, "logical_header_merge_func.c")
int a = 1; int observed = 701;

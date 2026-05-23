// RUN: %clang-refold-tester-with-lines conditional_join_imported_object_like
#include "loc_defs.h"
#if 1
#line LOC
int inserted = 0;
int inside = 1;
#endif
#line 502 "logical_imported.c"
int observed = __LINE__;

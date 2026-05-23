// RUN: %clang-refold-tester-with-lines header_interior_object_like
#include "loc_defs.h"
#if 1
#line LOC
int first = 0;
int inserted = 0;
#line 501 "logical_imported.c"
int inside = __LINE__;
#endif
#line 503 "logical_imported.c"
int observed = __LINE__;

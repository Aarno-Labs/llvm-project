// RUN: %clang-refold-tester-with-lines conditional_join_imported_object_like
#include "loc_defs.h"
#if 1
#line LOC
int inside = 1;
#endif
int observed = __LINE__;

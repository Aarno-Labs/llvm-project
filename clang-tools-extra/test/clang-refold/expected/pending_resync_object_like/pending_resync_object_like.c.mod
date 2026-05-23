// RUN: %clang-refold-tester-with-lines pending_resync_object_like
#include "loc_defs.h"
#line LOC
int a = 1; int b = 2;
#line 502 "logical_imported.c"
int observed = __LINE__;

// RUN: %clang-refold-tester-with-lines header_mid_arm_object_like
#include "loc_defs.h"
int prefix = 0;
int inserted = 0;
#line 3 "headers/h_mid_prefix_obj.h"
#if 1
#line LOC
int inside = __LINE__;
#endif
int observed = __LINE__;

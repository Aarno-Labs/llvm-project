// RUN: %clang-refold-tester-with-lines include_owned_define_after_placement_include_survivor
int M(void);
extern int keep; long before =
#line 1 "headers/include_owned_define_after_placement_include_survivor_def.h"
M()
#define M() 10
#line 2 "headers/include_owned_define_after_placement_include_survivor_def.h"

#line 5 "include_owned_define_after_placement_include_survivor.c"
;

#include "function_like_define_liveness_include_survivor.h"

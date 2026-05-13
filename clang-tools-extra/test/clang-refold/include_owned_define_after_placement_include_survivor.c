// RUN: %clang-refold-tester-with-lines include_owned_define_after_placement_include_survivor
int M(void);
extern int keep; int before =
#include "include_owned_define_after_placement_include_survivor_def.h"
;

#include "function_like_define_liveness_include_survivor.h"

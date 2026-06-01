// RUN: %clang-refold-tester-with-lines include_owned_define_after_placement_include_survivor
int M(void);
extern int keep; long before =
M()
#define M() 10
;

#include "function_like_define_liveness_include_survivor.h"

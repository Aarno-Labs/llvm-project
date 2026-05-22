// RUN: %clang-refold-tester-with-lines after_placement_define_liveness_include_survivor
int M(void);
extern int keep; long before =
M();
#define M() 10

#include "function_like_define_liveness_include_survivor.h"

// RUN: %clang-refold-tester-with-lines after_placement_define_liveness_include_survivor
int M(void);
extern int keep; int before =
#define M() 10
1;

#include "function_like_define_liveness_include_survivor.h"

// RUN: %clang-refold-tester-with-lines function_like_define_liveness_include_survivor
int
#define M() 10
a;

#include "function_like_define_liveness_include_survivor.h"

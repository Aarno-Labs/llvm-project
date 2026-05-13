// RUN: %clang-refold-tester-with-lines function_like_define_liveness_include_survivor
#define M() 10
long
M;
#line 5 "function_like_define_liveness_include_survivor.c"

#include "function_like_define_liveness_include_survivor.h"

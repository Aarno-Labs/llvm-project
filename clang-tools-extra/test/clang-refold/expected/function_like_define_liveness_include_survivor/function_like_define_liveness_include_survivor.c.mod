// RUN: %clang-refold-tester-with-lines function_like_define_liveness_include_survivor
#define M() 10
long
M;

#include "function_like_define_liveness_include_survivor.h"

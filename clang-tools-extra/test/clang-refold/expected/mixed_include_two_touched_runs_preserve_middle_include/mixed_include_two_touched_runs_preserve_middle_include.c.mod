// RUN: %clang-refold-tester-with-lines mixed_include_two_touched_runs_preserve_middle_include
#define KEEP(x) ((x) + 1)
int before = KEEP(1);
int left = 10;
#include "middle_value.h"
int right = 20;
int after = KEEP(2);

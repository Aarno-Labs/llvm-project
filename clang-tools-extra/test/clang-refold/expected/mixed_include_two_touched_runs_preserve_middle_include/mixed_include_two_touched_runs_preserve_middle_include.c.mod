// RUN: %clang-refold-tester-with-lines mixed_include_two_touched_runs_preserve_middle_include
#define KEEP(x) ((x) + 1)
int before = KEEP(1);
#line 1 "headers/left_value.h"
int left = 10;
#line 5 "mixed_include_two_touched_runs_preserve_middle_include.c"
#include "middle_value.h"
#line 1 "headers/right_value.h"
int right = 20;
#line 7 "mixed_include_two_touched_runs_preserve_middle_include.c"
int after = KEEP(2);

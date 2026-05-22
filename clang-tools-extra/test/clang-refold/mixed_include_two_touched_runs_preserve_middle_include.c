// RUN: %clang-refold-tester-with-lines mixed_include_two_touched_runs_preserve_middle_include
#define KEEP(x) ((x) + 1)
int before = KEEP(1);
#include "left_value.h"
#include "middle_value.h"
#include "right_value.h"
int after = KEEP(2);

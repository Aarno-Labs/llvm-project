// RUN: %clang-refold-tester-with-lines source_line_gap_include_materialization_with_line_suffix
#define KEEP(x) ((x) + 1)
int before = KEEP(1);
#line 100 "logical_input.h"
#line 1 "headers/line_value.h"
int line_value = 12;
#line 101 "logical_input.h"
int after = __LINE__;

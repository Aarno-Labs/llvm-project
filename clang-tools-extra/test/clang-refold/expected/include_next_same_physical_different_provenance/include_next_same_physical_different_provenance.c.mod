// RUN: %clang-refold-tester-clang-flags include_next_same_physical_different_provenance -- -I headers/t03/a -I headers/t03/b
int parent_value_t03 = 301;
#include "child.h"
int total_t03 = parent_value_t03 + child_value_t03 + leaf_value_t03;

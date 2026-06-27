// RUN: %clang-refold-tester-clang-flags include_next_descendant_clean_child -- -I headers/t01/a -I headers/t01/b
int parent_value_t01 = 11;
#include <child.h>
int total_t01 = parent_value_t01 + child_value_t01 + leaf_value_t01;

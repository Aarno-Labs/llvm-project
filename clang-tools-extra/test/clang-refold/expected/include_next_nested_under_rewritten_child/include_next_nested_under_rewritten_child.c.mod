// RUN: %clang-refold-tester-clang-flags include_next_nested_under_rewritten_child -- -I headers/t07/a -I headers/t07/b
int parent_value_t07 = 701;
#include "nested/child.h"
int use_t07 = parent_value_t07 + child_value_t07 + leaf_value_t07;

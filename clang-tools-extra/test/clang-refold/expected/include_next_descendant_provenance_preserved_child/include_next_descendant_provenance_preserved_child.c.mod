// RUN: %clang-refold-tester-clang-flags include_next_descendant_provenance_preserved_child -- -I headers/t08/a -I headers/t08/b
int parent_value_t08 = 801;
#include "child.h"
int use_t08 = parent_value_t08 + child_value_t08 + leaf_value_t08;

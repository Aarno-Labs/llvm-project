// RUN: %clang-refold-tester-clang-flags include_next_descendant_provenance_preserved_child -- -I headers/t08/a -I headers/t08/b
#include <parent.h>
int use_t08 = parent_value_t08 + child_value_t08 + leaf_value_t08;

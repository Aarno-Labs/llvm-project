// RUN: %clang-refold-tester-clang-flags include_next_direct_relocated -- -I headers/t02/a -I headers/t02/b
#include <shim.h>
int total_t02 = shim_value_t02 + leaf_value_t02;

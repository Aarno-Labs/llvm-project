// RUN: %clang-refold-tester-clang-flags include_next_i_a_i_b_ordering -- -I headers/t05/a -I headers/t05/b
#include <shim.h>
int use_t05 = shim_value_t05 + leaf_value_t05;

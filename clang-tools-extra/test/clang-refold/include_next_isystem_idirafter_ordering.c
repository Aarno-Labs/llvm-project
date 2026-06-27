// RUN: %clang-refold-tester-clang-flags include_next_isystem_idirafter_ordering -- -isystem headers/t06/sys -idirafter headers/t06/after
#include <shim.h>
int use_t06 = shim_value_t06 + leaf_value_t06;

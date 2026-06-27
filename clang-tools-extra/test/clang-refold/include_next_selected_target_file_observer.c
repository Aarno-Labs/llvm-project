// RUN: %clang-refold-tester-clang-flags include_next_selected_target_file_observer -- -I headers/t04/a -I headers/t04/b
#include <shim.h>
const char *use_file_t04 = leaf_file_t04;
int use_value_t04 = leaf_value_t04 + shim_value_t04;

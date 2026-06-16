// RUN: %clang-refold-tester-clang-flags-with-lines include_spelling_relative_search_dir_file_observer -- -I headers/include_spelling_relative
#include "parent.h"
int main(void) { return p + q + (v[0] != 0); }

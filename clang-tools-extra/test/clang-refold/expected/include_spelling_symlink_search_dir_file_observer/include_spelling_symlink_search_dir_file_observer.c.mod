// RUN: %clang-refold-tester-clang-flags-with-lines include_spelling_symlink_search_dir_file_observer -- -I headers/include_spelling_symlink/link
int p = 3;
#include "child.h"
int q = 2;
int main(void) { return p + q + (v[0] != 0); }

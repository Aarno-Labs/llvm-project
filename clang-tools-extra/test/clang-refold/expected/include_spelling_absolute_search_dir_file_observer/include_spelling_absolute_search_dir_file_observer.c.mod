// RUN: rm -rf /tmp/clang-refold-include-spelling-absolute-search-dir-file-observer && mkdir -p /tmp/clang-refold-include-spelling-absolute-search-dir-file-observer/headers && cp %S/headers/include_spelling_absolute/parent.h /tmp/clang-refold-include-spelling-absolute-search-dir-file-observer/headers/parent.h && cp %S/headers/include_spelling_absolute/child.h /tmp/clang-refold-include-spelling-absolute-search-dir-file-observer/headers/child.h
// RUN: %clang-refold-tester-clang-flags-with-lines include_spelling_absolute_search_dir_file_observer -- -I /tmp/clang-refold-include-spelling-absolute-search-dir-file-observer/headers
int p = 3;
#include "child.h"
int q = 2;
int main(void) { return p + q + (v[0] != 0); }

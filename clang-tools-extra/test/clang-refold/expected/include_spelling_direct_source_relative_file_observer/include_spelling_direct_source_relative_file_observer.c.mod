// RUN: %clang-refold-tester-clang-flags-with-lines include_spelling_direct_source_relative_file_observer -- -I .
int p = 3;
#include "headers/include_spelling_direct/child.h"
int q = 2;
int main(void) { return p + q + (v[0] != 0); }

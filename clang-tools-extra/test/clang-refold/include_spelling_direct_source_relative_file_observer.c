// RUN: %clang-refold-tester-clang-flags-with-lines include_spelling_direct_source_relative_file_observer -- -I .
#include "headers/include_spelling_direct/parent.h"
int main(void) { return p + q + (v[0] != 0); }

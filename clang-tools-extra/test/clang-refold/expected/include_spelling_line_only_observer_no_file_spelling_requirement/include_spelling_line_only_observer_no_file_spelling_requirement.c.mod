// RUN: %clang-refold-tester-clang-flags-with-lines include_spelling_line_only_observer_no_file_spelling_requirement -- -I headers/include_spelling_line
int p = 3;
#include "line_child.h"
int q = 2;
int main(void) { return p + q + line_value; }

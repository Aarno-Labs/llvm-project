// RUN: %clang-refold-tester-clang-flags-with-lines include_spelling_line_only_observer_no_file_spelling_requirement -- -I headers/include_spelling_line
#include "headers/include_spelling_line/parent.h"
int main(void) { return p + q + line_value; }

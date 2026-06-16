// RUN: %clang-refold-tester clean_child_stable_quoted_file_spelling_observer
int p = 3;
#include "child.h"
int q = 2;
int main(void) { return p + q; }

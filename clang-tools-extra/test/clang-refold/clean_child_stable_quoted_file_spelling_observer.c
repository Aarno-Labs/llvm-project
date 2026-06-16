// RUN: %clang-refold-tester clean_child_stable_quoted_file_spelling_observer
#include "headers/parent2.h"
int main(void) { return p + q; }

// RUN: %clang-refold-tester clean_child_stable_quoted_include_level_observer
#include "headers/parent3.h"
int main(void) { return p + v + q; }

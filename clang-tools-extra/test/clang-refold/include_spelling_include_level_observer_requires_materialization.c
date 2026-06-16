// RUN: %clang-refold-tester-clang-flags include_spelling_include_level_observer_requires_materialization -- -I headers/include_spelling_level
#include "headers/include_spelling_level/parent.h"
int main(void) { return p + q + level; }

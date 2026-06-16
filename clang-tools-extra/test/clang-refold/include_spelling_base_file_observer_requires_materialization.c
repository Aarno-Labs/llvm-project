// RUN: %clang-refold-tester-clang-flags include_spelling_base_file_observer_requires_materialization -- -I headers/include_spelling_base
#include "headers/include_spelling_base/parent.h"
int main(void) { return p + q + (base[0] != 0); }

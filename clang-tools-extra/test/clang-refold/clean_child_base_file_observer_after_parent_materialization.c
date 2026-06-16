// RUN: %clang-refold-tester-clang-flags clean_child_base_file_observer_after_parent_materialization -- -I %S
#include "headers/parent6.h"
int main(void) { return p + q; }

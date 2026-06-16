// RUN: %clang-refold-tester-clang-flags clean_child_symlink_macro_file_observer_after_parent_materialization -- -I %S
#include "headers/real2/parent2.h"
int main(void) { return p + q; }

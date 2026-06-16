// RUN: %clang-refold-tester-clang-flags clean_child_symlink_macro_file_observer_after_parent_materialization -- -I %S
int p = 3;
#include "headers/real2/child2.h"
int q = 2;
int main(void) { return p + q; }

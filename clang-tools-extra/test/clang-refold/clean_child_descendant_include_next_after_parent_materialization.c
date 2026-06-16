// RUN: %clang-refold-tester-clang-flags clean_child_descendant_include_next_after_parent_materialization -- -I %S/headers/descendant_include_next/a -I %S/headers/descendant_include_next/b
#include "headers/descendant_include_next/a/parent.h"
int main(void) { return p + q + x; }

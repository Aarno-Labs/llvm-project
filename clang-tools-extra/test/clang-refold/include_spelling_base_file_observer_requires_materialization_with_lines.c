// RUN: %clang-refold-tester-clang-flags-with-lines include_spelling_base_file_observer_requires_materialization_with_lines -- -I headers/include_spelling_base
// Line-control variant of
// include_spelling_base_file_observer_requires_materialization.
//
// The child is reached through an `-I` search path rather than a directory
// spelling, so its include operand cannot be replayed from the materialized
// parent surface.  `__BASE_FILE__` is unaffected by that spelling either way:
// the observer is discharged by realizing the child from B, in both line modes.
#include "headers/include_spelling_base/parent.h"
int main(void) { return p + q + (base[0] != 0); }

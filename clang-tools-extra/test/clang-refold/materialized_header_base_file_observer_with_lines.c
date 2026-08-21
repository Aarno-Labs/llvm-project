// RUN: %clang-refold-tester-clang-flags-with-lines materialized_header_base_file_observer_with_lines -- -I %S
// Line-control variant of materialized_header_base_file_observer.
//
// `__BASE_FILE__` names the top of the *presumed* include stack.  The edit to
// `p` forces the header to be materialized, and materialization deletes the
// include edge, so in the flattened output the walk stops at the spelling
// itself.  The include-entry `#line` the with-lines layout installs for the
// copied body therefore does not repair the observer -- it would make it read
// the header path.  The header body must be realized from B instead, exactly
// as it is under `--no-lines`; the two modes agree on this one.
#include "headers/parent5.h"
int main(void) { return p + q; }

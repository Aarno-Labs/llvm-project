// RUN: %clang-refold-tester-clang-flags-with-lines clean_child_base_file_observer_after_parent_materialization_with_lines -- -I %S
// Line-control variant of
// clean_child_base_file_observer_after_parent_materialization.
//
// The child include is lookup-stable across its parent's materialization, but
// its subtree observes `__BASE_FILE__`, so the clean-child replay proof vetoes
// preserving the directive and the child is materialized too.  Its body then
// carries a preserved `__BASE_FILE__` spelling into a flattened surface, which
// no include-entry `#line` can repair, so the child is realized from B.
int p = 3;
const char *v = "clean_child_base_file_observer_after_parent_materialization_with_lines.c";
int q = 2;
int main(void) { return p + q; }

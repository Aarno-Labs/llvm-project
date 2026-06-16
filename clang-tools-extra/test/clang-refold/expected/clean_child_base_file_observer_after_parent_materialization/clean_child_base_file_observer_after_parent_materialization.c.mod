// RUN: %clang-refold-tester-clang-flags clean_child_base_file_observer_after_parent_materialization -- -I %S
int p = 3;
const char *v = "clean_child_base_file_observer_after_parent_materialization.c";
int q = 2;
int main(void) { return p + q; }

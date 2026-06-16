// RUN: %clang-refold-tester-clang-flags clean_child_descendant_include_next_after_parent_materialization -- -I %S/headers/descendant_include_next/a -I %S/headers/descendant_include_next/b
int p = 3;
int x = 100;
int q = 2;
int main(void) { return p + q + x; }

// RUN: %clang-refold-tester-clang-flags clean_child_rewritten_quoted_include_level_observer -- -I %S
int p = 3;
int v = 2;
int q = 2;
int main(void) { return p + v + q; }

// RUN: %clang-refold-tester clean_child_stable_quoted_include_level_observer
int p = 3;
int v = 2;
int q = 2;
int main(void) { return p + v + q; }

// RUN: %clang-refold-tester-clang-flags clean_child_angled_include_next_relocated -- -I %S/headers/first -I %S/headers/second
int p = 3;
int v = 20;
int q = 2;
int main(void) { return p + v + q; }

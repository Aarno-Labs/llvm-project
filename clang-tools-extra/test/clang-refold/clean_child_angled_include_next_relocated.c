// RUN: %clang-refold-tester-clang-flags clean_child_angled_include_next_relocated -- -I %S/headers/first -I %S/headers/second
#include <parent.h>
int main(void) { return p + v + q; }

// RUN: %clang-refold-tester clean_child_angled_include_level_observer
#include <parent4.h>
int main(void) { return p + v + q; }

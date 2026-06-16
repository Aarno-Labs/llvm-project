// RUN: %clang-refold-tester-clang-flags clean_child_rewritten_quoted_include_level_observer -- -I %S
#include "headers/parent3.h"
int main(void) { return p + v + q; }

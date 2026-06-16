// RUN: %clang-refold-tester-clang-flags clean_child_operand_rewrite_targets_comment -- -I %S
int p = 3;
#include /* "leaf.h" */ "headers/leaf.h"
int q = 2;
int main(void) { return p + v + q; }

// RUN: %clang-refold-tester-clang-flags materialized_parent_include_operand_comment_shadow -- -I %S
int p = 3;
#include /* "leaf.h" */ "headers/leaf.h"
int q = 2;
int main(void) { return p + v + q; }

// RUN: %clang-refold-tester-clang-flags materialized_parent_include_operand_comment_shadow -- -I %S
#include "headers/parent.h"
int main(void) { return p + v + q; }

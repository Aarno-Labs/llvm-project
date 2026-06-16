// RUN: %clang-refold-tester-clang-flags clean_child_operand_rewrite_targets_comment -- -I %S
#include "headers/parent.h"
int main(void) { return p + v + q; }

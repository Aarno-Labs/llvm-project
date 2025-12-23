// RUN: %clang-refold-tester pure_ins_nested_cond_boundary1 XXX YYY
#include "l.h"

int main() {
  return FOO(5);
}

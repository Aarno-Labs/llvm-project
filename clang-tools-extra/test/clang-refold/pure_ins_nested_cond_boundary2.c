// RUN: %clang-refold-tester pure_ins_nested_cond_boundary2 XXX YYY ZZZ
#include "m.h"

int main() {
  return FOO(5);
}

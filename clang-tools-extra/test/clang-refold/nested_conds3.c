// RUN: %clang-refold-tester nested_conds3
// RUN: %clang-refold-tester nested_conds3 XXX
// RUN: %clang-refold-tester nested_conds3 XXX YYY
// RUN: %clang-refold-tester nested_conds3 XXX ZZZ
#include "a.h"
#include "g.h"
#include "b.h"

int main() {
  return 0;
}

// RUN: %clang-refold-tester nested_conds2
// RUN: %clang-refold-tester nested_conds2 XXX
// RUN: %clang-refold-tester nested_conds2 XXX YYY
// RUN: %clang-refold-tester nested_conds2 XXX ZZZ
#include "a.h"
#include "f.h"
#include "b.h"

int main() {
  return 0;
}

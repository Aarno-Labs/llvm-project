// RUN: %clang-refold-tester nested_conds1
// RUN: %clang-refold-tester nested_conds1 XXX
// RUN: %clang-refold-tester nested_conds1 XXX YYY
// RUN: %clang-refold-tester nested_conds1 XXX ZZZ
#include "a.h"
#include "f.h"
#include "b.h"

int main() {
  return 0;
}

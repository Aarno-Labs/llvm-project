// RUN: %clang-refold-tester inc_to_cond_to_include_boundary XXX YYY
#include "n.h"

int main() {
  return FOO(5);
}

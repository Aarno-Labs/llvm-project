// RUN: %clang-refold-tester inc_nested_in_cond
#include "d.h"
#include "e.h"

#define FOO(X) X

int main() {
  int y = 2;
  int z = FOO(3);
  char q = FOO('q');

  return q == '?' : y + z : 0;
}

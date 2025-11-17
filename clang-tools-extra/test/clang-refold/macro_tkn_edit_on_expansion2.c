// RUN: %clang-refold-tester macro_tkn_edit_on_expansion2
#include "i.h"

int main() {
  MAC(foo, 2);
  foo[0] = 5;
  foo[1] = 10;
}

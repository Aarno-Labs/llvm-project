// RUN: %clang-refold-tester macro_arg_edit_no_expansion1
#include "i.h"

int main() {
  MAC(foo, 2);
  foo[0] = 5;
  foo[1] = 10;
}

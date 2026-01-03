// RUN: %clang-refold-tester macro_arg_edit_no_expansion1
#include "i.h"

int main() {
  MAC(bob, 3);
  bob[0] = 5;
  bob[1] = 10;
  bob[2] = 15;
}

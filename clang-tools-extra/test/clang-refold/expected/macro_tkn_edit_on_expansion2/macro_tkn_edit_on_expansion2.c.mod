// RUN: %clang-refold-tester macro_tkn_edit_on_expansion2
#include "i.h"

int main() {
  int bob[3]
  bob[0] = 5;
  bob[1] = 10;
  bob[2] = 15;
}

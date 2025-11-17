// RUN: %clang-refold-tester macro_tkn_edit_on_expansion1
#include "h.h"
MAC(foo)

int main() {
  return 0;
}

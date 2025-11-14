// RUN: %clang-refold-tester nonmacro_tkn_edit_on_expansion
#include "h.h"
MAC(foo)

int main() {
  return 0;
}

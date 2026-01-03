// RUN: %clang-refold-tester macro_body_edit_on_expansion
#include "h.h"
MAC(foo)

int main() {
  return 0;
}

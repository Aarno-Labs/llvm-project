// RUN: %clang-refold-tester macro_arg_and_body_edit
// RUN: %clang-refold-tester macro_arg_and_body_edit XXX
#include "a.h"
#include "d.h"
#include "c.h"

#ifdef YYY
#define HELLO(X) (X * X)
#else
#define HELLO(X) (X + 2)
#endif

int main() {
  return HELLO(2) + HELLO(5);
}

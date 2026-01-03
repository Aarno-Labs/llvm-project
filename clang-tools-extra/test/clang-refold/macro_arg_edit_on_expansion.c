// RUN: %clang-refold-tester macro_arg_edit_on_expansion
#include "a.h"
#include "b.h"
#include "c.h"

#ifdef YYY
#define HELLO(X) (X * X)
#else
#define HELLO(X) (X + 2)
#endif

int main() {
  return HELLO(2) + HELLO(5);
}

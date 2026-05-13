// RUN: %clang-refold-tester include_closure_preserve_empty_conditional_gap
#define FOO(X,Y) ((X)+(Y))

int keep(void) {
  return FOO(1,2);
}

int x[] = {
#include "x.h"
#if 1
#endif
#include "z.h"
};

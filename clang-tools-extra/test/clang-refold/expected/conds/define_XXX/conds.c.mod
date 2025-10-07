// RUN: %clang-refold-tester conds
// RUN: %clang-refold-tester conds XXX
#include "a.h"
#pragma once

#ifdef XXX
int first(int x);
#define BAR(X) (X * 3)
#define FOO(X) (X + X)
int last(int x);
#else
float first(float x);
#define FOO(X) X
float last(float x);
#endif
#include "c.h"

#ifdef YYY
#define HELLO(X) (X * X)
#else
#define HELLO(X) (X + 2)
#endif

int main() {
  return HELLO(2) + (4 + 2);
}

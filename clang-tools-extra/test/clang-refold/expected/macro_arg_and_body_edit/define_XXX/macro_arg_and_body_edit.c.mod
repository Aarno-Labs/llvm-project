// RUN: %clang-refold-tester macro_arg_and_body_edit
// RUN: %clang-refold-tester macro_arg_and_body_edit XXX
#include "a.h"
#pragma once

#ifdef XXX
int first(int x);
#define FOO(X) (X + X)
#define BAR(X) (X * 3)
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
  return HELLO(2) + HELLO(4);
}

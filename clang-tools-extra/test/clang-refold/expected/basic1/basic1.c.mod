// RUN: %clang-refold-tester basic1
#include "a.h"
#include "b.h"
int foo(int x);
#define GOODBYE(X) (X * 3)
int bar(int x, int y);

#ifdef YYY
#define HELLO(X) (X * X)
#else
#define HELLO(X) (X + 2)
#endif

int main() {
  return (2 + 3) + HELLO(5);
}

// RUN: %clang-refold-tester macro_arg_and_body_edit
// RUN: %clang-refold-tester macro_arg_and_body_edit XXX
#include "a.h"


#ifdef XXX
int first(int x);
#define FOO(X) (X + X)
int last(int x);
#else
float first(float x);
#define FOO(X) X
#define BAR(X) (X * 3)
float last(float x);
#endif
#include "c.h"

#ifdef YYY
#define HELLO(X) (X * X)
#else
#define HELLO(X) (X + 2)
#endif

int main() {
  return (2 + 3) + HELLO(5);
}

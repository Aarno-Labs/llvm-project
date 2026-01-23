// RUN: %clang-refold-tester inc_nested_in_cond
#include "d.h"
#include "a.h"
#ifdef XXX
#include "b.h"
#else
int foo(int x);
void fakeFn(const char *str, int idx);
int bar(int x, int y);
#endif

#define FOO(X) X

int main() {
  int y = 102;
  int z = FOO(3);
  char q = FOO('a');

  return q == '?' : y + z : 0;
}

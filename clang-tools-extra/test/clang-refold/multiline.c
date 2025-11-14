// RUN: %clang-refold-tester multiline
struct T { int* pa; int b; int* px; int y; };

int ig;
struct T g = { .pa = &g.b, .y = 4, .b = 3, .px = &g.b };

#include "b.h"
int main() {
  *(g.pa) = 32;
  *(g.px) += 10;
  printf("%d\n", *(g.pa));
  return 0;
}

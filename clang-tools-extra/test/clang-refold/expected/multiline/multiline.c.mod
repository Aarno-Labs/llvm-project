// RUN: %clang-refold-tester multiline
struct T { int* pa; int b; int* px; int y; };

int ig;
struct T g = { .pa = &g.b, .y = 4, .b = 3, .px = &g.b };

int first(int x);
// Type definitions needed for XjGlobals

struct XjGlobals {
};
int last(int x);
int main() { struct XjGlobals xjgv = {
  };
  struct XjGlobals *xjg = &xjgv;
  *(g.pa) = 32;
  *(g.px) += 10;
  printf("%d\n", *(g.pa));
  return 0;
}

// RUN: %clang-refold-tester basic2 CCC
int main() {
#ifdef CCC
  int x = 1;
#endif
  int y = 2;
  int z = x + y;
  return z;
}

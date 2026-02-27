// RUN: %clang-refold-tester-with-lines macro_arg_rewrite_complex_arg_commas_braces

// test65: Complex arg containing commas/braces; stress arg-range parsing.
#define WRAP(X) X
int g65(int a, int b) { return a + b; }
int main() {
  int x = WRAP(g65(1, 99));
  int y = WRAP(((int[]){3,5})[0]);
  return x + y;
}

// RUN: %clang-refold-tester undef_preserve_function_like
#define FOO(X, Y) ((X)+(Y))
#undef FOO
int main(void) {
  return FOO(1, 3);
}

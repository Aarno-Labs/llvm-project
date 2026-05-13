// RUN: %clang-refold-tester undef_preserve_function_like
#define FOO(X, Y) ((X)+(Y))
int before = 1;
#undef FOO
int after = 2;

int main(void) {
  return FOO(1, 3);
}

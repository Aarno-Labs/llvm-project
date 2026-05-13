// RUN: %clang-refold-tester define_preserve_function_like
#define FOO(x,y) ((x)*(y))
int main(void) {
  return FOO(3, 2);
}

// RUN: %clang-refold-tester define_preserve_function_like
int before = 1;
#define FOO(x,y) ((x)*(y))
int after = 2;

int main(void) {
  return FOO(3, 2);
}

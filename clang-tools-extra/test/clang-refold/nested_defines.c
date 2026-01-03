// RUN: %clang-refold-tester nested_defines
#define BAR 5
#define FOO BAR

int main() {
  printf("%d\n", FOO);
  return 0;
}

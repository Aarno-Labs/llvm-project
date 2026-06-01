// RUN: %clang-refold-tester-with-lines bracket_split_wrapper_chain
int arr[10];

#define SUM3(a, b, c) int x = a + b + c;
#define BUILD_SUM(a, b, c) SUM3(a, b, c)
#define WRAP_SUM(a, b, c) BUILD_SUM(a, b, c)

void f(void) {
  WRAP_SUM(arr[1, 2], 3)
}

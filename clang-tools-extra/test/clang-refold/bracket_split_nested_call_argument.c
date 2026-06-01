// RUN: %clang-refold-tester-with-lines bracket_split_nested_call_argument
int arr[100];

#define IDX(a, b) a + b
#define SUM3(a, b, c) int x = a + b + c;
#define BUILD_SUM(a, b, c) SUM3(a, b, c)
#define WRAP_SUM(a, b, c) BUILD_SUM(a, b, c)

void f(void) {
  WRAP_SUM(arr[IDX(1, 2), 3], 4)
}

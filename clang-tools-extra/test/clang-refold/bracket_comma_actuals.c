// RUN: %clang-refold-tester-with-lines bracket_comma_actuals
int arr[10];
#define SUM3(a, b, c) int x = a + b + c;
void f(void) {
  SUM3(arr[1, 2], 3)
}

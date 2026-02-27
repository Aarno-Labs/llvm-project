// RUN: %clang-refold-tester-with-lines gnu_comma_eating_va_args_through_nesting

// test105.c: GNU ,##__VA_ARGS__ behavior through nesting.
// (Clang supports this GNU extension.)
//#include <stdio.h>

#define P(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define P1(...) P(__VA_ARGS__)
#define P2(...) P1(__VA_ARGS__)

int main(void) {
  P2("hello\n");
  P2("n=%d\n", 7);
  return 0;
}

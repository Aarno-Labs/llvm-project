// RUN: %clang-refold-tester-with-lines varargs_forwarded_through_two_macros

// test104.c: varargs forwarded through two macros.
//#include <stdio.h>

#define VLOG(fmt, ...) printf((fmt), __VA_ARGS__)
#define LOG1(...) VLOG(__VA_ARGS__)
#define LOG2(...) LOG1(__VA_ARGS__)

int main(void) {
  printf(("x=%d y=%d\n"), 10, 20);
  return 0;
}

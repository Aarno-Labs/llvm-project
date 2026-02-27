// RUN: %clang-refold-tester-with-lines varargs_implicit_va_args

// test77: implicit varargs.
//#include <stdio.h>
#define LOG(fmt, ...) printf(fmt, __VA_ARGS__)
int main(){ LOG("%d %d\n", 1, 2); return 0; }

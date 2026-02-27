// RUN: %clang-refold-tester-with-lines varargs_comma_eating_hashhash_va_args

// test79: comma-eating ##__VA_ARGS__.
//#include <stdio.h>
#define LOG3(fmt, ...) printf(fmt, ##__VA_ARGS__)
int main(){ LOG3("HELLO\n"); LOG3("%d\n", 6); return 0; }

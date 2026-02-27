// RUN: %clang-refold-tester-with-lines varargs_gnu_named_args_ellipsis

// test78: explicit GNU args...
//#include <stdio.h>
#define LOG2(fmt, args...) printf(fmt, args)
int main(){ LOG2("%d,%d\n", 3, 4); return 0; }

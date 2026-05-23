// RUN: %clang-refold-tester-with-lines line_min_nested_macro_line_observer FIRST
#line 700 "line_min_nested_macro_line_observer.c"
#define A() __LINE__
#define B() A()
int anchor = 0;
int observed = B();

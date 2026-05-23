// RUN: %clang-refold-tester-with-lines line_min_forwarded_macro_line_observer FIRST
#line 800 "line_min_forwarded_macro_line_observer.c"
#define ID(x) x
int anchor = 0;
int observed = ID(__LINE__);

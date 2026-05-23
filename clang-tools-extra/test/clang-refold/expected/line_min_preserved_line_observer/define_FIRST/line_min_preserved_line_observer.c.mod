// RUN: %clang-refold-tester-with-lines line_min_preserved_line_observer FIRST
#line 900 "line_min_preserved_line_observer.c"
int anchor = __LINE__;
int inserted = 0;
#line 901 "line_min_preserved_line_observer.c"
int suffix = __LINE__;

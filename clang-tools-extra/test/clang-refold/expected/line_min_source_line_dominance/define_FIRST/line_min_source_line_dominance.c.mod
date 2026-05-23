// RUN: %clang-refold-tester-with-lines line_min_source_line_dominance FIRST
#line 1 "dead-prefix.c"
#line 300 "live-observer.c"
int observed = __LINE__;
int payload = 1;

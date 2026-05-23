// RUN: %clang-refold-tester-with-lines line_min_macro_line_observer FIRST
#line 400 "line_min_macro_line_observer.c"
#define OBS_LINE() __LINE__
int anchor = 0;
int observed = OBS_LINE();

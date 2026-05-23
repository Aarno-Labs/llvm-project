// RUN: %clang-refold-tester-with-lines line_min_macro_file_observer FIRST
#line 30 "/virtual/root/line_min_macro_file_observer.c"
#define OBS_FILE() __FILE__
int payload = 1;
const char *file = OBS_FILE();

// RUN: %clang-refold-tester-with-lines line_min_macro_filename_observer FIRST
#line 30 "/virtual/root/line_min_macro_filename_observer.c"
#define OBS_FILENAME() __FILE_NAME__
int payload = 0;
const char *file = OBS_FILENAME();

// RUN: %clang-refold-tester-with-lines macro_caller_tu_prefix
// Prefix line collapse before an observer reached through OBS().
#define LOC 600 "logical_macro_tu_prefix.c"
#define OBS() __LINE__
#line LOC
int a = 1; int b = 2;
#line 602 "logical_macro_tu_prefix.c"
int observed = OBS();

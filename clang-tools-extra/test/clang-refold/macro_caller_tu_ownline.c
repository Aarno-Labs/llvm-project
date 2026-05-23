// RUN: %clang-refold-tester-with-lines macro_caller_tu_ownline
// Observable __LINE__ is reached through OBS(), not direct source spelling.
#define LOC 500 "logical_macro_tu_ownline.c"
#define OBS() __LINE__
#line LOC
int a = 1;
int observed = OBS();

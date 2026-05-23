// RUN: %clang-refold-tester-with-lines header_macro_ownline
#define LOC 700 "logical_macro_header_ownline.c"
#define OBS() __LINE__
#line LOC
int a = 1; int observed = 701;

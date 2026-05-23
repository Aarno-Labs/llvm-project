// RUN: %clang-refold-tester-with-lines header_macro_prefix
#define LOC 800 "logical_macro_header_prefix.c"
#define OBS() __LINE__
#line LOC
int a = 1; int b = 2;
#line 802 "logical_macro_header_prefix.c"
int observed = OBS();

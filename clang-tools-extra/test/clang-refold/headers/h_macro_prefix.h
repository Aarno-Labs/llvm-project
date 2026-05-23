#define LOC 800 "logical_macro_header_prefix.c"
#define OBS() __LINE__
#line LOC
int a = 1;
int b = 2;
int observed = OBS();

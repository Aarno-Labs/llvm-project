#define LOC 900 "logical_prefix_left_macro_header.c"
#define B_STMT int b = 2;
#line LOC
int a = 1;
B_STMT
int observed = __LINE__;

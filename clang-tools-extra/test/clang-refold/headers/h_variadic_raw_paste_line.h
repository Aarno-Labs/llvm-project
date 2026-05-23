int head = 1;
#define RAW 8
#define RAW0 980
#define LOC(...) __VA_ARGS__ ## 0 "variadic_raw_paste_header.c"
#line LOC(RAW)
int value = __LINE__;
const char *file = __FILE__;

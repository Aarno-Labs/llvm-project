#define LOC(n, name, ...) n __VA_OPT__(#name)
#line LOC(1410, vaopt_stringified_header.c, present)
int head = 1;
int value = __LINE__;
const char *file = __FILE__;

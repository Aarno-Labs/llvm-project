#define LOC 995 "base_elif_header.c"
#if 1 ? 0 : 1
#define LOC 195 "inactive_if_arm_header.c"
#elif 1
#define LOC 995 "active_elif_header.c"
#endif
#line LOC
int head = __LINE__;
int value = __LINE__;
const char *file = __FILE__;

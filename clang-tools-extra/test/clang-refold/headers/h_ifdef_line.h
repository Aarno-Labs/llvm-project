#define LOC 945 "active_ifdef_header.c"
#ifdef NEVER_DEFINED_FOR_HEADER_LINE_CONTROL
#define LOC 145 "inactive_ifdef_header.c"
#endif
#line LOC
int value = __LINE__;
const char *file = __FILE__;

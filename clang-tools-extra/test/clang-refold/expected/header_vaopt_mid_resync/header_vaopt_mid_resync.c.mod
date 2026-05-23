// RUN: %clang-refold-tester-with-lines header_vaopt_mid_resync
#line 1 "headers/h_vaopt_mid.h"
#define LOC(n, ...) n __VA_OPT__(__VA_ARGS__)
#line LOC(1310, "vaopt_header.c")
int head = 1;
int inserted = 0;
#line 1311 "vaopt_header.c"
int value = __LINE__;
const char *file = __FILE__;
#line 3 "header_vaopt_mid_resync.c"
int tail = 3;

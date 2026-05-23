// RUN: %clang-refold-tester-with-lines header_vaopt_mid_resync
#define LOC(n, ...) n __VA_OPT__(__VA_ARGS__)
#line LOC(1310, "vaopt_header.c")
int head = 1;
int inserted = 0;
#line 1311 "vaopt_header.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;

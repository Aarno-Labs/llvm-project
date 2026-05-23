// RUN: %clang-refold-tester-with-lines tu_vaopt_line_control_resync
#define LOC(n, ...) n __VA_OPT__(__VA_ARGS__)
#line LOC(1300, "vaopt_tu.c")
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;

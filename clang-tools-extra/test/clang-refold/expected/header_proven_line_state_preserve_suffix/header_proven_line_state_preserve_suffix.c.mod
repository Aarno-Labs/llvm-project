// RUN: %clang-refold-tester-with-lines header_proven_line_state_preserve_suffix
int inserted = 0;
#define HEAD_TAKE_ACTIVE_LINE 1
#if HEAD_TAKE_ACTIVE_LINE
#  line 990 "active_only_header.c"
#else
#  line 1200 "inactive_only_header.c"
#endif
int head_value = 994;
const char *head_file = __FILE__;
int tail = 3;

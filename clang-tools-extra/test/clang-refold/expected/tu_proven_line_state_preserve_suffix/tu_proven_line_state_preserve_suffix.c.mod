#line 1 "tu_proven_line_state_preserve_suffix.c"
// RUN: %clang-refold-tester-with-lines tu_proven_line_state_preserve_suffix
#define TAKE_ACTIVE_LINE 1
#if TAKE_ACTIVE_LINE
#  line 700 "active_only_main.c"
#else
#  line 900 "inactive_only_main.c"
#endif
int inserted = 0;
int value = 704;
const char *file = __FILE__;
int tail = 3;

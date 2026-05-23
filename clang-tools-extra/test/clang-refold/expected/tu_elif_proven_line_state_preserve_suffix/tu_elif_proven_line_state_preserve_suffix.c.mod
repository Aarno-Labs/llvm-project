#line 1 "tu_elif_proven_line_state_preserve_suffix.c"
// RUN: %clang-refold-tester-with-lines tu_elif_proven_line_state_preserve_suffix
#define TAKE_ELIF_LINE 1
#if 0
#  line 610 "inactive_if_main.c"
#elif TAKE_ELIF_LINE
#  line 830 "active_elif_only_main.c"
#else
#  line 960 "inactive_else_main.c"
#endif
int inserted = 0;
int value = 835;
const char *file = __FILE__;
int tail = 3;

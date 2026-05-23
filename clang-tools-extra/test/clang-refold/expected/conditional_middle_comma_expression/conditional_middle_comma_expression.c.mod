// RUN: %clang-refold-tester-with-lines conditional_middle_comma_expression
#define DEAD_LINE 9001
#define LIVE_LINE 700

#if 1 ? 0, 0 : 1
#  define LINE_TOKEN DEAD_LINE
#  line LINE_TOKEN "dead-middle-comma.c"
int selected = __LINE__;
#else
#  define LINE_TOKEN LIVE_LINE
#  line LINE_TOKEN "live-middle-comma.c"
int selected = __LINE__;
#endif
int selected_extra = 701;
#line 702 "live-middle-comma.c"
int suffix = __LINE__;

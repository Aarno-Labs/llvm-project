// RUN: %clang-refold-tester-with-lines conditional_skipped_branch_divzero
#define DEAD_LINE 9001
#define LIVE_LINE 700

#if 1 ? 0 : (1 / 0)
#  define LINE_TOKEN DEAD_LINE
#  line LINE_TOKEN "dead-conditional-skip.c"
int selected = __LINE__;
#else
#  define LINE_TOKEN LIVE_LINE
#  line LINE_TOKEN "live-conditional-skip.c"
int selected = __LINE__;
#endif
int selected_extra = 701;
#line 702 "live-conditional-skip.c"
int suffix = __LINE__;

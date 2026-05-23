// RUN: %clang-refold-tester-with-lines line_ternary_selected_branch
#define DEAD_LINE 9001
#define LIVE_LINE 700

#if 1 ? (1 ? (1 - 1) : 1) : 0
#  define LINE_TOKEN DEAD_LINE
#  line LINE_TOKEN "dead-arm.c"
int selected_value = __LINE__;
#else
#  define LINE_TOKEN LIVE_LINE
#  line LINE_TOKEN "live-arm.c"
int selected_value = __LINE__;
#endif
int selected_extra = 701;
#line 702 "live-arm.c"

#line LINE_TOKEN "post-branch.c"
int post_value = __LINE__;

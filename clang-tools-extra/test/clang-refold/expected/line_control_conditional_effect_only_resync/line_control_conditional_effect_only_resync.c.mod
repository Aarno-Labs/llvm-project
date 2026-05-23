// RUN: %clang-refold-tester-with-lines line_control_conditional_effect_only_resync
#define USE 1
#if USE
#line 900 "cond_line_only.c"
#else
#line 100 "wrong.c"
#endif
#line 904 "cond_line_only.c"
int keep = __LINE__;
